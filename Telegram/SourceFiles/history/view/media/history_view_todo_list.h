/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/media/history_view_media.h"
#include "ui/effects/animations.h"
#include "data/data_todo_list.h"
#include "base/weak_ptr.h"

#include <QtGui/QRegion>
#include <QtGui/QTransform>

namespace Ui {
class RippleAnimation;
class FireworksAnimation;
} // namespace Ui

namespace HistoryView {

class Message;

class TodoList final : public Media {
public:
	TodoList(
		not_null<Element*> parent,
		not_null<TodoListData*> todolist,
		Element *replacing);
	~TodoList();

	void draw(Painter &p, const PaintContext &context) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	bool toggleSelectionByHandlerClick(const ClickHandlerPtr &p) const override {
		return true;
	}
	bool dragItemByHandler(const ClickHandlerPtr &p) const override {
		return true;
	}

	bool needsBubble() const override {
		return true;
	}
	bool customInfoLayout() const override {
		return false;
	}

	[[nodiscard]] TextSelection adjustSelection(
		TextSelection selection,
		TextSelectType type) const override;
	uint16 fullSelectionLength() const override;
	TextForMimeData selectedText(TextSelection selection) const override;

	void paintBubbleFireworks(
		Painter &p,
		const QRect &bubble,
		crl::time ms) const override;

	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;

	void unloadHeavyPart() override;
	bool hasHeavyPart() const override;

	void hideSpoilers() override;

	std::vector<TodoTaskInfo> takeTasksInfo() override;

private:
	struct Task;
	enum class TaskRepaintPart {
		Text,
		Toggle,
		Ripple,
	};
	struct RepaintState {
		uint64 generation = 0;
		QRegion current;
		QRegion stale;
		uint32 pending : 1 = 0;
		uint32 known : 1 = 0;
	};
	struct TaskRepaints {
		int id = 0;
		RepaintState text;
		RepaintState toggle;
		RepaintState ripple;
	};

	QSize countOptimalSize() override;
	QSize countCurrentSize(int newWidth) override;

	[[nodiscard]] bool canComplete() const;

	[[nodiscard]] int countTaskTop(
		const Task &task,
		int innerWidth) const;
	[[nodiscard]] int countTaskHeight(
		const Task &task,
		int innerWidth) const;
	[[nodiscard]] ClickHandlerPtr createTaskClickHandler(
		const Task &task);
	void fillTaskData(Task &task, const TodoListItem &original);
	void updateTexts();
	void updateTasks(bool skipAnimations);
	void startToggleAnimation(Task &task);
	void updateCompletionStatus();
	void maybeStartFireworks();
	void setupPreviousState(const std::vector<TodoTaskInfo> &info);

	int paintTask(
		Painter &p,
		const Task &task,
		int left,
		int top,
		int width,
		int outerWidth,
		const PaintContext &context) const;
	void paintRadio(
		Painter &p,
		const Task &task,
		int left,
		int top,
		const PaintContext &context) const;
	void paintStatus(
		Painter &p,
		const Task &task,
		int left,
		int top,
		const PaintContext &context) const;
	void paintBottom(
		Painter &p,
		int left,
		int top,
		int paintw,
		const PaintContext &context) const;
	void appendTaskHighlight(
		int id,
		int top,
		int height,
		const PaintContext &context) const;
	void recordTextRepaint(
		RepaintState &repaint,
		const Painter &p,
		const PaintContext &context,
		const Ui::Text::String &text,
		QRect rect,
		const Ui::Text::CustomEmojiPaintedBounds
			&customEmojiPaintedBounds) const;
	void recordTaskRepaint(
		int id,
		TaskRepaintPart part,
		const Painter &p,
		const PaintContext &context,
		const QRegion &region) const;
	void recordRepaintGeometry(
		RepaintState &repaint,
		QRegion region,
		bool known) const;
	void invalidateRepaintGeometry(RepaintState &repaint) const;
	void invalidateRepaintGeometries() const;
	void repaintTitle(uint64 generation) const;
	void repaintTask(
		int id,
		TaskRepaintPart part,
		uint64 generation) const;
	void repaintFireworks(uint64 generation) const;
	void repaintRegion(const QRegion &region) const;
	[[nodiscard]] TaskRepaints &ensureTaskRepaints(int id) const;
	[[nodiscard]] TaskRepaints *findTaskRepaints(int id) const;
	[[nodiscard]] RepaintState &taskRepaint(
		TaskRepaints &repaints,
		TaskRepaintPart part) const;
	[[nodiscard]] uint64 resetTaskRepaint(
		int id,
		TaskRepaintPart part);
	void resetTaskRipple(const Task &task) const;
	void removeMissingTaskRepaints() const;
	void rememberElementPaint(
		const Painter &p,
		const PaintContext &context) const;
	[[nodiscard]] std::optional<QRect> mapCurrentPaintToElement(
		const Painter &p,
		QRectF rect) const;

	void radialAnimationCallback() const;

	void toggleRipple(Task &task, bool pressed);
	void toggleCompletion(int id);

	[[nodiscard]] int bottomButtonHeight() const;

	const not_null<TodoListData*> _todolist;
	int _todoListVersion = 0;
	int _total = 0;
	int _incompleted = 0;
	TodoListData::Flags _flags = TodoListData::Flags();

	mutable RepaintState _titleRepaint;
	mutable RepaintState _fireworksRepaint;
	mutable std::vector<TaskRepaints> _taskRepaints;
	uint64 _nextRepaintGeneration = 0;
	mutable QSize _repaintLayoutSize;
	mutable const QPaintDevice *_elementPaintDevice = nullptr;
	mutable std::optional<QTransform> _elementTransform;
	mutable const QPaintDevice *_lastDrawPaintDevice = nullptr;
	mutable uint32 _lastDrawCanonical : 1 = 0;

	Ui::Text::String _title;
	Ui::Text::String _subtitle;

	std::vector<Task> _tasks;
	Ui::Text::String _completionStatusLabel;

	mutable std::unique_ptr<Ui::FireworksAnimation> _fireworksAnimation;
	mutable QPoint _lastLinkPoint;
	mutable QImage _userpicCircleCache;
	mutable QImage _fillingIconCache;

};

} // namespace HistoryView
