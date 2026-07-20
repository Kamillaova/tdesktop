/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_todo_list.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "core/ui_integration.h" // TextContext
#include "lang/lang_keys.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_message.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_text_helper.h"
#include "calls/calls_instance.h"
#include "ui/chat/message_bubble.h"
#include "ui/chat/chat_style.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/text/format_values.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/fireworks_animation.h"
#include "ui/toast/toast.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "data/data_media_types.h"
#include "data/data_poll.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "base/unixtime.h"
#include "base/timer.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "api/api_todo_lists.h"
#include "window/window_peer_menu.h"
#include "styles/style_chat.h"
#include "styles/style_polls.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

namespace HistoryView {
namespace {

[[nodiscard]] bool HasTodoTextAnimation(const Ui::Text::String &text) {
	return text.hasCustomEmoji() || text.hasSpoilers();
}

[[nodiscard]] QMargins TodoTextRepaintMargins() {
	const auto inner = st::emojiSize;
	const auto outer = Ui::Text::AdjustCustomEmojiSize(inner);
	const auto skip = (inner - outer) / 2;
	const auto before = std::max(-skip, 0);
	const auto after = std::max(skip + outer - inner, 0);
	return { before, before, after, after };
}

[[nodiscard]] bool AddTodoTextRepaintRegion(
		QRegion &region,
		const Painter &p,
		const PaintContext &context,
		const Ui::Text::String &text,
		QRect rect) {
	const auto customEmoji = text.hasCustomEmoji();
	if (!HasTodoTextAnimation(text)) {
		return true;
	} else if (rect.isEmpty()) {
		return true;
	}
	const auto layoutWidth = std::min(rect.width(), text.maxWidth());
	const auto lines = text.countLinesGeometry(layoutWidth);
	if (lines.empty()) {
		return true;
	}
	const auto margins = customEmoji
		? TodoTextRepaintMargins()
		: QMargins();
	auto mappedRegion = QRegion();
	auto lineTop = 0;
	for (const auto &line : lines) {
		const auto lineBottom = std::clamp(
			line.bottom,
			lineTop,
			rect.height());
		auto lineLeft = std::clamp(line.left, 0, layoutWidth);
		const auto lineWidth = std::clamp(
			line.width,
			0,
			layoutWidth - lineLeft);
		if (line.rtl) {
			lineLeft = layoutWidth - lineLeft - lineWidth;
		}
		if (lineWidth > 0 && lineBottom > lineTop) {
			auto lineRect = QRectF(
				rect.x() + lineLeft,
				rect.y() + lineTop,
				lineWidth,
				lineBottom - lineTop);
			if (customEmoji) {
				lineRect = lineRect.marginsAdded(QMarginsF(margins));
			}
			const auto mapped = context.mapToElement(p, lineRect);
			if (!mapped || mapped->isEmpty()) {
				return false;
			}
			mappedRegion += *mapped;
		}
		lineTop = lineBottom;
		if (lineTop == rect.height()) {
			break;
		}
	}
	region += mappedRegion;
	return true;
}

} // namespace

struct TodoList::Task {
	Task();

	void fillData(
		not_null<Element*> view,
		not_null<TodoListData*> todolist,
		const TodoListItem &original,
		Ui::Text::MarkedContext context);
	void setCompletedBy(PeerData *by);

	Ui::Text::String text;
	Ui::Text::String name;
	PeerData *completedBy = nullptr;
	mutable Ui::PeerUserpicView userpic;
	TimeId completionDate = 0;
	int id = 0;
	ClickHandlerPtr handler;
	Ui::Animations::Simple selectedAnimation;
	mutable std::unique_ptr<Ui::RippleAnimation> ripple;
	mutable QSize rippleMaskSize;
};

TodoList::Task::Task()
: text(st::msgMinWidth / 2)
, name(st::msgMinWidth / 2) {
}

void TodoList::Task::fillData(
		not_null<Element*> view,
		not_null<TodoListData*> todolist,
		const TodoListItem &original,
		Ui::Text::MarkedContext context) {
	id = original.id;
	setCompletedBy(original.completedBy);
	completionDate = original.completionDate;
	if (!text.isEmpty() && text.toTextWithEntities() == original.text) {
		return;
	}
	text.setMarkedText(
		st::historyPollAnswerStyle,
		original.text,
		Ui::WebpageTextTitleOptions(),
		context);
	InitElementTextPart(view, text);
}

void TodoList::Task::setCompletedBy(PeerData *by) {
	if (!by || completedBy == by) {
		return;
	}
	completedBy = by;
	name.setText(st::historyPollAnswerStyle, completedBy->name());
}

TodoList::TodoList(
	not_null<Element*> parent,
	not_null<TodoListData*> todolist,
	Element *replacing)
: Media(parent)
, _todolist(todolist)
, _title(st::msgMinWidth / 2) {
	history()->owner().registerTodoListView(_todolist, _parent);
	if (const auto media = replacing ? replacing->media() : nullptr) {
		const auto info = media->takeTasksInfo();
		if (!info.empty()) {
			setupPreviousState(info);
		}
	}
}

void TodoList::setupPreviousState(const std::vector<TodoTaskInfo> &info) {
	// If we restore state from the view we're replacing we'll be able to
	// animate the changes properly.
	updateTasks(true);
	for (auto &task : _tasks) {
		const auto i = ranges::find(info, task.id, &TodoTaskInfo::id);
		if (i != end(info)) {
			task.setCompletedBy(i->completedBy);
			task.completionDate = i->completionDate;
		}
	}
}

QSize TodoList::countOptimalSize() {
 	updateTexts();

	const auto paddings = st::msgPadding.left() + st::msgPadding.right();

	auto maxWidth = st::msgFileMinWidth;
	accumulate_max(maxWidth, paddings + _title.maxWidth());
	for (const auto &task : _tasks) {
		accumulate_max(
			maxWidth,
			paddings
			+ st::historyChecklistTaskPadding.left()
			+ task.text.maxWidth()
			+ st::historyChecklistTaskPadding.right());
	}

	const auto tasksHeight = ranges::accumulate(ranges::views::all(
		_tasks
	) | ranges::views::transform([](const Task &task) {
		return st::historyChecklistTaskPadding.top()
			+ task.text.minHeight()
			+ st::historyChecklistTaskPadding.bottom();
	}), 0);

	const auto bottomButtonHeight = st::historyPollBottomButtonSkip;
	auto minHeight = st::historyPollQuestionTop
		+ _title.minHeight()
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip
		+ tasksHeight
		+ st::historyPollTotalVotesSkip
		+ bottomButtonHeight
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
	if (!isBubbleTop()) {
		minHeight -= st::msgFileTopMinus;
	}
	return { maxWidth, minHeight };
}

bool TodoList::canComplete() const {
	return (_parent->data()->out()
		|| _parent->history()->peer->isSelf()
		|| _todolist->othersCanComplete())
		&& _parent->data()->isRegular()
		&& !_parent->data()->Has<HistoryMessageForwarded>();
}

int TodoList::countTaskTop(
		const Task &task,
		int innerWidth) const {
	auto tshift = st::historyPollQuestionTop;
	if (!isBubbleTop()) {
		tshift -= st::msgFileTopMinus;
	}
	tshift += _title.countHeight(innerWidth) + st::historyPollSubtitleSkip;
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;
	const auto i = ranges::find(
		_tasks,
		&task,
		[](const Task &task) { return &task; });
	const auto countHeight = [&](const Task &task) {
		return countTaskHeight(task, innerWidth);
	};
	tshift += ranges::accumulate(
		begin(_tasks),
		i,
		0,
		ranges::plus(),
		countHeight);
	return tshift;
}

int TodoList::countTaskHeight(
		const Task &task,
		int innerWidth) const {
	const auto answerWidth = innerWidth
		- st::historyChecklistTaskPadding.left()
		- st::historyChecklistTaskPadding.right();
	return st::historyChecklistTaskPadding.top()
		+ task.text.countHeight(answerWidth)
		+ st::historyChecklistTaskPadding.bottom();
}

TodoList::TaskRepaints &TodoList::ensureTaskRepaints(int id) const {
	const auto existing = findTaskRepaints(id);
	if (existing) {
		return *existing;
	}
	return _taskRepaints.emplace_back(TaskRepaints{ .id = id });
}

TodoList::TaskRepaints *TodoList::findTaskRepaints(int id) const {
	const auto i = ranges::find(_taskRepaints, id, &TaskRepaints::id);
	return (i == end(_taskRepaints)) ? nullptr : &*i;
}

TodoList::RepaintState &TodoList::taskRepaint(
		TaskRepaints &repaints,
		TaskRepaintPart part) const {
	switch (part) {
	case TaskRepaintPart::Text: return repaints.text;
	case TaskRepaintPart::Toggle: return repaints.toggle;
	case TaskRepaintPart::Ripple: return repaints.ripple;
	}
	Unexpected("Task repaint part in TodoList::taskRepaint.");
}

uint64 TodoList::resetTaskRepaint(
		int id,
		TaskRepaintPart part) {
	auto &repaint = taskRepaint(ensureTaskRepaints(id), part);
	repaint.pending = false;
	repaint.generation = ++_nextRepaintGeneration;
	return repaint.generation;
}

void TodoList::resetTaskRipple(const Task &task) const {
	if (const auto repaints = findTaskRepaints(task.id)) {
		repaints->ripple.generation = 0;
		repaints->ripple.pending = false;
	}
	task.ripple.reset();
	task.rippleMaskSize = QSize();
}

void TodoList::invalidateRepaintGeometry(RepaintState &repaint) const {
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.known = false;
}

void TodoList::invalidateRepaintGeometries() const {
	invalidateRepaintGeometry(_titleRepaint);
	invalidateRepaintGeometry(_fireworksRepaint);
	for (auto &repaints : _taskRepaints) {
		invalidateRepaintGeometry(repaints.text);
		invalidateRepaintGeometry(repaints.toggle);
		invalidateRepaintGeometry(repaints.ripple);
	}
}

void TodoList::repaintRegion(const QRegion &region) const {
	for (const auto &rect : region) {
		_parent->repaint(rect);
	}
}

void TodoList::recordRepaintGeometry(
		RepaintState &repaint,
		QRegion region,
		bool known) const {
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(
		base::take(repaint.current));
	repaint.pending = false;
	repaint.current = known ? std::move(region) : QRegion();
	repaint.known = known;
	if (!known) {
		repaint.stale = previous;
		if (!previous.isEmpty()) {
			repaint.pending = true;
			this->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = true;
	repaintRegion(previous.united(repaint.current));
}

void TodoList::recordTextRepaint(
		RepaintState &repaint,
		const Painter &p,
		const PaintContext &context,
		const Ui::Text::String &text,
		QRect rect) const {
	if (!context.hasElementPainter(p)) {
		return;
	}
	auto region = QRegion();
	const auto known = AddTodoTextRepaintRegion(
		region,
		p,
		context,
		text,
		rect);
	recordRepaintGeometry(repaint, std::move(region), known);
}

void TodoList::recordTaskRepaint(
		int id,
		TaskRepaintPart part,
		const Painter &p,
		const PaintContext &context,
		const QRegion &rects) const {
	if (!context.hasElementPainter(p)) {
		return;
	}
	auto region = QRegion();
	auto known = true;
	for (const auto &rect : rects) {
		const auto mapped = context.mapToElement(p, QRectF(rect));
		if (!mapped || mapped->isEmpty()) {
			known = false;
			break;
		}
		region += *mapped;
	}
	recordRepaintGeometry(
		taskRepaint(ensureTaskRepaints(id), part),
		std::move(region),
		known);
}

void TodoList::repaintTitle(uint64 generation) const {
	auto &repaint = _titleRepaint;
	if (generation != repaint.generation
		|| repaint.pending
		|| _parent->delegate()->elementAnimationsPaused()
		|| (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = true;
	if (repaint.known) {
		repaintRegion(repaint.current);
	} else {
		this->repaint();
	}
}

void TodoList::repaintTask(
		int id,
		TaskRepaintPart part,
		uint64 generation) const {
	const auto repaints = findTaskRepaints(id);
	if (!repaints) {
		return;
	}
	auto &repaint = taskRepaint(*repaints, part);
	if (generation != repaint.generation
		|| repaint.pending
		|| ((part == TaskRepaintPart::Text)
			&& _parent->delegate()->elementAnimationsPaused())
		|| (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = true;
	if (repaint.known) {
		repaintRegion(repaint.current);
	} else {
		this->repaint();
	}
}

void TodoList::repaintFireworks(uint64 generation) const {
	auto &repaint = _fireworksRepaint;
	if (generation != repaint.generation
		|| repaint.pending
		|| (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = true;
	if (repaint.known) {
		repaintRegion(repaint.current);
	} else {
		this->repaint();
	}
}

void TodoList::removeMissingTaskRepaints() const {
	for (auto i = begin(_taskRepaints); i != end(_taskRepaints);) {
		if (ranges::find(_tasks, i->id, &Task::id) != end(_tasks)) {
			++i;
			continue;
		}
		recordRepaintGeometry(i->text, QRegion(), true);
		recordRepaintGeometry(i->toggle, QRegion(), true);
		recordRepaintGeometry(i->ripple, QRegion(), true);
		i = _taskRepaints.erase(i);
	}
}

void TodoList::rememberElementPaint(
		const Painter &p,
		const PaintContext &context) const {
	_lastDrawPaintDevice = p.device();
	_lastDrawCanonical = context.hasElementPainter(p);
	if (!_lastDrawCanonical) {
		return;
	}
	_elementPaintDevice = p.device();
	_elementTransform = context.elementTransform;
}

std::optional<QRect> TodoList::mapCurrentPaintToElement(
		const Painter &p,
		QRectF rect) const {
	if (!_elementTransform || _elementPaintDevice != p.device()) {
		return std::nullopt;
	}
	auto invertible = false;
	const auto inverted = _elementTransform->inverted(&invertible);
	if (!invertible) {
		return std::nullopt;
	}
	return inverted.map(
		p.transform().map(QPolygonF(rect))
	).boundingRect().toAlignedRect();
}

QSize TodoList::countCurrentSize(int newWidth) {
	accumulate_min(newWidth, maxWidth());
	const auto innerWidth = newWidth
		- st::msgPadding.left()
		- st::msgPadding.right();

	auto tasksHeight = 0;
	for (const auto &task : _tasks) {
		const auto taskHeight = countTaskHeight(task, innerWidth);
		tasksHeight += taskHeight;
		if (task.ripple
			&& task.rippleMaskSize != QSize(newWidth, taskHeight)) {
			resetTaskRipple(task);
		}
	}

	const auto bottomButtonHeight = st::historyPollBottomButtonSkip;
	auto newHeight = st::historyPollQuestionTop
		+ _title.countHeight(innerWidth)
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip
		+ tasksHeight
		+ st::historyPollTotalVotesSkip
		+ bottomButtonHeight
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
	if (!isBubbleTop()) {
		newHeight -= st::msgFileTopMinus;
	}
	const auto result = QSize(newWidth, newHeight);
	if (_repaintLayoutSize != result) {
		invalidateRepaintGeometries();
		_repaintLayoutSize = result;
	}
	return result;
}

void TodoList::updateTexts() {
	if (_todoListVersion == _todolist->version) {
		return;
	}
	invalidateRepaintGeometries();
	const auto skipAnimations = _tasks.empty();
	_todoListVersion = _todolist->version;

	if (_title.toTextWithEntities() != _todolist->title) {
		_titleRepaint.generation = ++_nextRepaintGeneration;
		const auto generation = _titleRepaint.generation;
		const auto weak = base::make_weak(this);
		auto options = Ui::WebpageTextTitleOptions();
		options.maxw = options.maxh = 0;
		_title.setMarkedText(
			st::historyPollQuestionStyle,
			_todolist->title,
			options,
			Core::TextContext({
				.session = &_todolist->session(),
				.repaint = [=] {
					if (const auto strong = weak.get()) {
						strong->repaintTitle(generation);
					}
				},
				.customEmojiLoopLimit = 2,
			}));
		InitElementTextPart(_parent, _title);
	}
	if (_flags != _todolist->flags() || _subtitle.isEmpty()) {
		_flags = _todolist->flags();
		_subtitle.setText(
			st::msgDateTextStyle,
			(!_todolist->othersCanComplete()
				? tr::lng_todo_title(tr::now)
				: _parent->data()->history()->peer->isUser()
				? tr::lng_todo_title_user(tr::now)
				: tr::lng_todo_title_group(tr::now)));
	}
	updateTasks(skipAnimations);
}

void TodoList::fillTaskData(
		Task &task,
		const TodoListItem &original) {
	auto &repaint = ensureTaskRepaints(original.id).text;
	const auto textChanged = task.text.isEmpty()
		|| task.text.toTextWithEntities() != original.text;
	if (textChanged && task.ripple) {
		resetTaskRipple(task);
	}
	if (textChanged || !repaint.generation) {
		invalidateRepaintGeometry(repaint);
		repaint.generation = ++_nextRepaintGeneration;
	}
	const auto id = original.id;
	const auto generation = repaint.generation;
	const auto weak = base::make_weak(this);
	task.fillData(
		_parent,
		_todolist,
		original,
		Core::TextContext({
			.session = &_todolist->session(),
			.repaint = [=] {
				if (const auto strong = weak.get()) {
					strong->repaintTask(
						id,
						TaskRepaintPart::Text,
						generation);
				}
			},
			.customEmojiLoopLimit = 2,
		}));
}

void TodoList::updateTasks(bool skipAnimations) {
	const auto changed = !ranges::equal(
		_tasks,
		_todolist->items,
		ranges::equal_to(),
		&Task::id,
		&TodoListItem::id);
	if (!changed) {
		auto animated = false;
		auto &&tasks = ranges::views::zip(_tasks, _todolist->items);
		for (auto &&[task, original] : tasks) {
			const auto wasDate = task.completionDate;
			fillTaskData(task, original);
			if (!skipAnimations && (!wasDate != !task.completionDate)) {
				startToggleAnimation(task);
				animated = true;
			}
		}
		updateCompletionStatus();
		if (animated) {
			maybeStartFireworks();
		}
		return;
	}
	const auto has = hasHeavyPart();
	for (auto &repaints : _taskRepaints) {
		invalidateRepaintGeometry(repaints.text);
		invalidateRepaintGeometry(repaints.toggle);
		invalidateRepaintGeometry(repaints.ripple);
		repaints.text.generation = 0;
		repaints.toggle.generation = 0;
		repaints.ripple.generation = 0;
	}
	_tasks = ranges::views::all(
		_todolist->items
	) | ranges::views::transform([&](const TodoListItem &item) {
		auto result = Task();
		result.id = item.id;
		fillTaskData(result, item);
		return result;
	}) | ranges::to_vector;

	for (auto &task : _tasks) {
		task.handler = createTaskClickHandler(task);
	}

	updateCompletionStatus();

	if (has && !hasHeavyPart()) {
		_parent->checkHeavyPart();
	}
}

ClickHandlerPtr TodoList::createTaskClickHandler(
		const Task &task) {
	const auto id = task.id;
	auto result = std::make_shared<LambdaClickHandler>(crl::guard(this, [=] {
		toggleCompletion(id);
	}));
	result->setProperty(kTodoListItemIdProperty, id);
	return result;
}

void TodoList::startToggleAnimation(Task &task) {
	const auto selected = (task.completionDate != 0);
	const auto id = task.id;
	const auto generation = resetTaskRepaint(
		id,
		TaskRepaintPart::Toggle);
	const auto weak = base::make_weak(this);
	task.selectedAnimation.start(
		[=] {
			if (const auto strong = weak.get()) {
				strong->repaintTask(
					id,
					TaskRepaintPart::Toggle,
					generation);
			}
		},
		selected ? 0. : 1.,
		selected ? 1. : 0.,
		st::defaultCheck.duration);
}

void TodoList::toggleCompletion(int id) {
	if (_parent->data()->isBusinessShortcut()) {
		return;
	} else if (_parent->data()->Has<HistoryMessageForwarded>()) {
		_parent->delegate()->elementShowTooltip(
			tr::lng_todo_mark_forwarded(tr::now, tr::rich),
			[] {});
		return;
	} else if (!canComplete()) {
		_parent->delegate()->elementShowTooltip(
			tr::lng_todo_mark_restricted(
				tr::now,
				lt_user,
				tr::bold(_parent->data()->from()->shortName()),
				tr::rich), [] {});
		return;
	} else if (!_parent->history()->session().premium()) {
		Window::PeerMenuTodoWantsPremium(Window::TodoWantsPremium::Mark);
		return;
	}
	const auto i = ranges::find(
		_tasks,
		id,
		&Task::id);
	if (i == end(_tasks)) {
		return;
	}
	if (const auto repaints = findTaskRepaints(id)) {
		invalidateRepaintGeometry(repaints->text);
	}

	const auto selected = (i->completionDate != 0);
	i->completionDate = selected ? TimeId() : base::unixtime::now();
	if (!selected) {
		i->setCompletedBy(_parent->history()->session().user());
	}

	const auto parentMedia = _parent->data()->media();
	const auto baseList = parentMedia ? parentMedia->todolist() : nullptr;
	if (baseList) {
		const auto j = ranges::find(baseList->items, id, &TodoListItem::id);
		if (j != end(baseList->items)) {
			j->completionDate = i->completionDate;
			j->completedBy = i->completedBy;
		}
		history()->owner().updateDependentMessages(_parent->data());
	}

	startToggleAnimation(*i);
	repaint();

	history()->session().api().todoLists().toggleCompletion(
		_parent->data()->fullId(),
		id,
		!selected);

	maybeStartFireworks();
}

void TodoList::maybeStartFireworks() {
	if (!ranges::contains(_tasks, TimeId(), &Task::completionDate)
		&& !_fireworksAnimation) {
		_fireworksRepaint.pending = false;
		_fireworksRepaint.generation = ++_nextRepaintGeneration;
		const auto generation = _fireworksRepaint.generation;
		const auto weak = base::make_weak(this);
		_fireworksAnimation = std::make_unique<Ui::FireworksAnimation>(
			[=] {
				if (const auto strong = weak.get()) {
					strong->repaintFireworks(generation);
				}
			});
	}
}

void TodoList::updateCompletionStatus() {
	const auto incompleted = int(ranges::count(
		_todolist->items,
		nullptr,
		&TodoListItem::completedBy));
	const auto total = int(_todolist->items.size());
	if (_total == total
		&& _incompleted == incompleted
		&& !_completionStatusLabel.isEmpty()) {
		return;
	}
	_total = total;
	_incompleted = incompleted;
	const auto totalText = QString::number(total);
	const auto string = (incompleted == total)
		? tr::lng_todo_completed_none(tr::now, lt_total, totalText)
		: tr::lng_todo_completed(
			tr::now,
			lt_count,
			total - incompleted,
			lt_total,
			totalText);
	_completionStatusLabel.setText(st::msgDateTextStyle, string);
}

void TodoList::draw(Painter &p, const PaintContext &context) const {
	rememberElementPaint(p, context);
	if (_repaintLayoutSize != currentSize()) {
		invalidateRepaintGeometries();
		_repaintLayoutSize = currentSize();
	}
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		if (context.hasElementPainter(p)) {
			recordRepaintGeometry(_titleRepaint, QRegion(), true);
			for (auto &repaints : _taskRepaints) {
				recordRepaintGeometry(repaints.text, QRegion(), true);
				recordRepaintGeometry(repaints.toggle, QRegion(), true);
				recordRepaintGeometry(repaints.ripple, QRegion(), true);
			}
			removeMissingTaskRepaints();
		}
		return;
	}
	auto paintw = width();

	const auto stm = context.messageStyle();
	const auto padding = st::msgPadding;
	auto tshift = st::historyPollQuestionTop;
	if (!isBubbleTop()) {
		tshift -= st::msgFileTopMinus;
	}
	paintw -= padding.left() + padding.right();

	recordTextRepaint(
		_titleRepaint,
		p,
		context,
		_title,
		QRect(
			padding.left(),
			tshift,
			paintw,
			_title.countHeight(paintw)));
	p.setPen(stm->historyTextFg);
	_parent->prepareCustomEmojiPaint(
		p,
		context,
		_title,
		CustomEmojiRepaintReset::No);
	_title.draw(p, {
		.position = { padding.left(), tshift },
		.availableWidth = paintw,
		.palette = &stm->textPalette,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
		.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
		.selection = context.selection,
	});
	tshift += _title.countHeight(paintw) + st::historyPollSubtitleSkip;

	p.setPen(stm->msgDateFg);
	_subtitle.drawLeftElided(p, padding.left(), tshift, paintw, width());
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;

	auto heavy = false;
	auto created = false;
	auto &&tasks = ranges::views::zip(
		_tasks,
		ranges::views::ints(0, int(_tasks.size())));
	for (const auto &[task, index] : tasks) {
		const auto was = !task.userpic.null();
		const auto height = paintTask(
			p,
			task,
			padding.left(),
			tshift,
			paintw,
			width(),
			context);
		appendTaskHighlight(task.id, tshift, height, context);
		if (was) {
			heavy = true;
		} else if (!task.userpic.null()) {
			created = true;
		}
		tshift += height;
	}
	if (!heavy && created) {
		history()->owner().registerHeavyViewPart(_parent);
	}
	paintBottom(p, padding.left(), tshift, paintw, context);
	if (context.hasElementPainter(p)) {
		removeMissingTaskRepaints();
	}
}

void TodoList::paintBottom(
		Painter &p,
		int left,
		int top,
		int paintw,
		const PaintContext &context) const {
	const auto stringtop = top
		+ st::msgPadding.bottom()
		+ st::historyChecklistBottomTop;
	const auto stm = context.messageStyle();

	p.setPen(stm->msgDateFg);
	_completionStatusLabel.draw(p, left, stringtop, paintw, style::al_top);
}

void TodoList::radialAnimationCallback() const {
	if (!anim::Disabled()) {
		repaint();
	}
}

int TodoList::paintTask(
		Painter &p,
		const Task &task,
		int left,
		int top,
		int width,
		int outerWidth,
		const PaintContext &context) const {
	const auto height = countTaskHeight(task, width);
	const auto stm = context.messageStyle();
	const auto aleft = left + st::historyChecklistTaskPadding.left();
	const auto awidth = width
		- st::historyChecklistTaskPadding.left()
		- st::historyChecklistTaskPadding.right();
	const auto textTop = top + (task.completionDate
		? st::historyChecklistCheckedTop
		: st::historyChecklistTaskPadding.top());
	const auto radioTop = top + st::historyChecklistTaskPadding.top();
	const auto rippleMaskSize = QSize(outerWidth, height);
	if (task.ripple && task.rippleMaskSize != rippleMaskSize) {
		resetTaskRipple(task);
	}
	const auto radio = QRect(
		left,
		radioTop,
		st::historyPollRadio.diameter,
		st::historyPollRadio.diameter);
	const auto radioAdd = st::lineWidth * 3;
	const auto aaAdd = st::lineWidth;
	const auto margins = [](int add) {
		return QMargins(add, add, add, add);
	};
	const auto skip = st::lineWidth;
	const auto userpic = QRect(
		left + st::historyPollRadio.diameter / 2 + skip,
		radioTop + skip,
		st::historyPollRadio.diameter - 2 * skip,
		st::historyPollRadio.diameter - 2 * skip
	).marginsAdded(margins(aaAdd));
	const auto &chosen = stm->historyPollChosen;
	const auto chosenLeft = left
		+ (st::historyPollRadio.diameter - chosen.width()) / 2;
	const auto chosenTop = radioTop
		+ (st::historyPollRadio.diameter - chosen.height()) / 2;
	auto toggleRegion = QRegion(radio.marginsAdded(margins(radioAdd)));
	toggleRegion += userpic;
	toggleRegion += style::rtlrect(
		chosenLeft,
		chosenTop,
		chosen.width(),
		chosen.height(),
		outerWidth).marginsAdded(margins(aaAdd));
	recordTaskRepaint(
		task.id,
		TaskRepaintPart::Ripple,
		p,
		context,
		QRegion(QRect(
			left - st::msgPadding.left(),
			top,
			outerWidth,
			height)));
	recordTaskRepaint(
		task.id,
		TaskRepaintPart::Toggle,
		p,
		context,
		toggleRegion);
	recordTextRepaint(
		ensureTaskRepaints(task.id).text,
		p,
		context,
		task.text,
		QRect(
			aleft,
			textTop,
			awidth,
			task.text.countHeight(awidth)));

	if (task.ripple) {
		p.setOpacity(st::historyPollRippleOpacity);
		task.ripple->paint(
			p,
			left - st::msgPadding.left(),
			top,
			outerWidth,
			&stm->msgWaveformInactive->c);
		if (task.ripple->empty()) {
			resetTaskRipple(task);
		}
		p.setOpacity(1.);
	}

	if (canComplete()) {
		paintRadio(p, task, left, top, context);
	} else {
		paintStatus(p, task, left, top, context);
	}

	top = textTop;
	p.setPen(stm->historyTextFg);
	_parent->prepareCustomEmojiPaint(
		p,
		context,
		task.text,
		CustomEmojiRepaintReset::No);
	task.text.draw(p, {
		.position = { aleft, top },
		.availableWidth = awidth,
		.palette = &stm->textPalette,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
		.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
	});
	if (task.completionDate) {
		const auto nameTop = top
			+ height
			- st::historyChecklistTaskPadding.bottom()
			+ st::historyChecklistCheckedTop
			- st::normalFont->height;
		p.setPen(stm->msgDateFg);
		task.name.drawLeft(p, aleft, nameTop, awidth, outerWidth);
	}
	return height;
}

void TodoList::appendTaskHighlight(
		int id,
		int top,
		int height,
		const PaintContext &context) const {
	if (context.highlight.todoItemId != id
		|| context.highlight.collapsion <= 0.) {
		return;
	}
	const auto to = context.highlightInterpolateTo;
	const auto toProgress = (1. - context.highlight.collapsion);
	if (toProgress >= 1.) {
		context.highlightPathCache->addRect(to);
	} else if (toProgress <= 0.) {
		context.highlightPathCache->addRect(0, top, width(), height);
	} else {
		const auto lerp = [=](int from, int to) {
			return from + (to - from) * toProgress;
		};
		context.highlightPathCache->addRect(
			lerp(0, to.x()),
			lerp(top, to.y()),
			lerp(width(), to.width()),
			lerp(height, to.height()));
	}
}

void TodoList::paintRadio(
		Painter &p,
		const Task &task,
		int left,
		int top,
		const PaintContext &context) const {
	top += st::historyChecklistTaskPadding.top();

	const auto stm = context.messageStyle();

	PainterHighQualityEnabler hq(p);
	const auto &radio = st::historyPollRadio;
	const auto over = ClickHandler::showAsActive(task.handler);
	const auto &regular = stm->msgDateFg;

	const auto checkmark = task.selectedAnimation.value(
		task.completionDate ? 1. : 0.);

	const auto o = p.opacity();
	if (checkmark < 1.) {
		p.setBrush(Qt::NoBrush);
		p.setOpacity(o * (over ? st::historyPollRadioOpacityOver : st::historyPollRadioOpacity));
	}

	const auto rect = QRectF(left, top, radio.diameter, radio.diameter).marginsRemoved(QMarginsF(radio.thickness / 2., radio.thickness / 2., radio.thickness / 2., radio.thickness / 2.));
	if (checkmark > 0. && task.completedBy) {
		const auto skip = st::lineWidth;
		const auto userpic = QRect(
			left + (radio.diameter / 2) + skip,
			top + skip,
			radio.diameter - 2 * skip,
			radio.diameter - 2 * skip);
		if (checkmark < 1.) {
			p.save();
			p.setOpacity(checkmark);
			p.translate(QRectF(userpic).center());
			const auto ratio = 0.4 + 0.6 * checkmark;
			p.scale(ratio, ratio);
			p.translate(-QRectF(userpic).center());
		}
		task.completedBy->paintUserpic(
			p,
			task.userpic,
			userpic.left(),
			userpic.top(),
			userpic.width());
		if (checkmark < 1.) {
			p.restore();
		}
	}
	if (checkmark < 1.) {
		auto pen = regular->p;
		pen.setWidth(radio.thickness);
		p.setPen(pen);
		p.drawEllipse(rect);
	}

	if (checkmark > 0.) {
		const auto removeFull = (radio.diameter / 2 - radio.thickness);
		const auto removeNow = removeFull * (1. - checkmark);
		const auto color = stm->msgFileThumbLinkFg;
		auto pen = color->p;
		pen.setWidth(radio.thickness);
		p.setPen(pen);
		p.setBrush(color);
		p.drawEllipse(rect.marginsRemoved({ removeNow, removeNow, removeNow, removeNow }));
		const auto &icon = stm->historyPollChosen;
		icon.paint(p, left + (radio.diameter - icon.width()) / 2, top + (radio.diameter - icon.height()) / 2, width());

		const auto stm = context.messageStyle();
		auto bgpen = stm->msgBg->p;
		bgpen.setWidth(st::lineWidth);
		const auto outline = QRect(left, top, radio.diameter, radio.diameter);
		const auto paintContent = [&](QPainter &p) {
			p.setPen(bgpen);
			p.setBrush(Qt::NoBrush);
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(outline);
		};
		if (usesBubblePattern(context)) {
			const auto add = st::lineWidth * 3;
			const auto target = outline.marginsAdded(
				{ add, add, add, add });
			Ui::PaintPatternBubblePart(
				p,
				context.viewport,
				context.bubblesPattern->pixmap,
				target,
				paintContent,
				_userpicCircleCache);
		} else {
			paintContent(p);
		}
	}

	p.setOpacity(o);
}

void TodoList::paintStatus(
		Painter &p,
		const Task &task,
		int left,
		int top,
		const PaintContext &context) const {
	top += st::historyChecklistTaskPadding.top();

	const auto stm = context.messageStyle();

	const auto &radio = st::historyPollRadio;
	const auto completed = (task.completionDate != 0);

	const auto rect = QRect(left, top, radio.diameter, radio.diameter);
	if (completed) {
		const auto &icon = stm->historyPollChosen;
		icon.paint(
			p,
			left + (radio.diameter - icon.width()) / 2,
			top + (radio.diameter - icon.height()) / 2,
			width(),
			stm->msgFileBg->c);
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(stm->msgFileBg);

		PainterHighQualityEnabler hq(p);
		p.drawEllipse(style::centerrect(
			rect,
			QRect(0, 0, st::mediaUnreadSize, st::mediaUnreadSize)));
	}
}

TextSelection TodoList::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	return _title.adjustSelection(selection, type);
}

uint16 TodoList::fullSelectionLength() const {
	return _title.length();
}

TextForMimeData TodoList::selectedText(TextSelection selection) const {
	return _title.toTextForMimeData(selection);
}

TextState TodoList::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	const auto padding = st::msgPadding;
	auto paintw = width();
	auto tshift = st::historyPollQuestionTop;
	if (!isBubbleTop()) {
		tshift -= st::msgFileTopMinus;
	}
	paintw -= padding.left() + padding.right();

	const auto questionH = _title.countHeight(paintw);
	if (QRect(padding.left(), tshift, paintw, questionH).contains(point)) {
		result = TextState(_parent, _title.getState(
			point - QPoint(padding.left(), tshift),
			paintw,
			request.forText()));
		return result;
	}
	const auto aleft = padding.left()
		+ st::historyChecklistTaskPadding.left();
	const auto awidth = paintw
		- st::historyChecklistTaskPadding.left()
		- st::historyChecklistTaskPadding.right();
	tshift += questionH + st::historyPollSubtitleSkip;
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;
	for (const auto &task : _tasks) {
		const auto height = countTaskHeight(task, paintw);
		if (point.y() >= tshift && point.y() < tshift + height) {
			const auto atop = tshift
				+ (task.completionDate
					? st::historyChecklistCheckedTop
					: st::historyChecklistTaskPadding.top());
			auto taskTextResult = task.text.getState(
				point - QPoint(aleft, atop),
				awidth,
				request.forText());
			if (taskTextResult.link) {
				result.link = taskTextResult.link;
			} else {
				_lastLinkPoint = point;
				result.link = task.handler;
			}
			if (task.completionDate) {
				result.customTooltip = true;
				using Flag = Ui::Text::StateRequest::Flag;
				if (request.flags & Flag::LookupCustomTooltip) {
					result.customTooltipText = langDateTimeFull(
						base::unixtime::parse(task.completionDate));
				}
			}
			return result;
		}
		tshift += height;
	}
	return result;
}

void TodoList::paintBubbleFireworks(
		Painter &p,
		const QRect &bubble,
		crl::time ms) const {
	if (_lastDrawCanonical && _lastDrawPaintDevice == p.device()) {
		auto region = QRegion();
		auto known = true;
		if (!bubble.isEmpty()) {
			const auto mapped = mapCurrentPaintToElement(
				p,
				QRectF(bubble));
			if (!mapped || mapped->isEmpty()) {
				known = false;
			} else {
				region += *mapped;
			}
		}
		if (_fireworksAnimation) {
			recordRepaintGeometry(
				_fireworksRepaint,
				std::move(region),
				known);
		} else {
			_fireworksRepaint.current = known
				? std::move(region)
				: QRegion();
			_fireworksRepaint.stale = QRegion();
			_fireworksRepaint.pending = false;
			_fireworksRepaint.known = known;
		}
	}
	if (!_fireworksAnimation) {
		return;
	}
	if (_fireworksAnimation->paint(p, bubble)) {
		return;
	}
	_fireworksRepaint.generation = 0;
	_fireworksAnimation = nullptr;
	_fireworksRepaint.pending = false;
}

void TodoList::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (!handler) return;

	const auto i = ranges::find(
		_tasks,
		handler,
		&Task::handler);
	if (i != end(_tasks)) {
		toggleRipple(*i, pressed);
	}
}

void TodoList::unloadHeavyPart() {
	_title.unloadPersistentAnimation();
	for (auto &task : _tasks) {
		task.userpic = {};
		task.text.unloadPersistentAnimation();
	}
}

bool TodoList::hasHeavyPart() const {
	for (auto &task : _tasks) {
		if (!task.userpic.null()) {
			return true;
		}
	}
	return false;
}

void TodoList::hideSpoilers() {
	if (_title.hasSpoilers()) {
		_title.setSpoilerRevealed(false, anim::type::instant);
	}
	for (auto &task : _tasks) {
		if (task.text.hasSpoilers()) {
			task.text.setSpoilerRevealed(false, anim::type::instant);
		}
	}
}

std::vector<Media::TodoTaskInfo> TodoList::takeTasksInfo() {
	if (_tasks.empty()) {
		return {};
	}
	return _tasks | ranges::views::transform([](const Task &task) {
		return TodoTaskInfo{
			.id = task.id,
			.completedBy = task.completedBy,
			.completionDate = task.completionDate,
		};
	}) | ranges::to_vector;
}

void TodoList::toggleRipple(Task &task, bool pressed) {
	if (pressed) {
		const auto outerWidth = width();
		const auto innerWidth = outerWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		const auto maskSize = QSize(
			outerWidth,
			countTaskHeight(task, innerWidth));
		if (task.ripple && task.rippleMaskSize != maskSize) {
			resetTaskRipple(task);
		}
		if (!task.ripple) {
			auto mask = Ui::RippleAnimation::RectMask(maskSize);
			const auto id = task.id;
			const auto generation = resetTaskRepaint(
				id,
				TaskRepaintPart::Ripple);
			const auto weak = base::make_weak(this);
			task.rippleMaskSize = maskSize;
			task.ripple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				std::move(mask),
				[=] {
					if (const auto strong = weak.get()) {
						strong->repaintTask(
							id,
							TaskRepaintPart::Ripple,
							generation);
					}
				});
		}
		const auto top = countTaskTop(task, innerWidth);
		task.ripple->add(_lastLinkPoint - QPoint(0, top));
	} else if (task.ripple) {
		task.ripple->lastStop();
	}
}

int TodoList::bottomButtonHeight() const {
	const auto skip = st::historyPollChoiceRight.height()
		- st::historyPollFillingBottom
		- st::historyPollFillingHeight
		- (st::historyPollChoiceRight.height() - st::historyPollFillingHeight) / 2;
	return st::historyPollTotalVotesSkip
		- skip
		+ st::historyPollBottomButtonSkip
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
}

TodoList::~TodoList() {
	_titleRepaint.generation = 0;
	_fireworksRepaint.generation = 0;
	for (auto &repaints : _taskRepaints) {
		repaints.text.generation = 0;
		repaints.toggle.generation = 0;
		repaints.ripple.generation = 0;
	}
	history()->owner().unregisterTodoListView(_todolist, _parent);
	if (hasHeavyPart()) {
		unloadHeavyPart();
		_parent->checkHeavyPart();
	}
}

} // namespace HistoryView
