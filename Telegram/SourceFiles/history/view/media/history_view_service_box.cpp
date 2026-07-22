/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_service_box.h"

#include "core/ui_integration.h"
#include "data/data_session.h"
#include "history/view/media/history_view_sticker_player_abstract.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_text_helper.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "ui/chat/chat_style.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/premium_stars_colored.h"
#include "ui/effects/ripple_animation.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/text/text_utilities.h"
#include "ui/damage_debug.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/rect.h"
#include "styles/style_chat.h"
#include "styles/style_credits.h"
#include "styles/style_polls.h"
#include "styles/style_premium.h"
#include "styles/style_layers.h"

namespace HistoryView {
namespace {

[[nodiscard]] bool AddTextRepaintBounds(
		QRegion &region,
		const Painter &p,
		const PaintContext &context,
		const Ui::Text::String &text,
		QRectF textRect,
		const Ui::Text::CustomEmojiRepaintBounds &customEmojiBounds) {
	auto repaintRect = customEmojiBounds.rect;
	if (!customEmojiBounds.repaintBoundsKnown) {
		if (textRect.isEmpty()) {
			return false;
		}
		repaintRect = repaintRect.united(textRect);
	} else if (text.hasSpoilers() && !textRect.isEmpty()) {
		repaintRect = repaintRect.united(textRect);
	}
	if (!repaintRect.isEmpty()) {
		const auto mapped = context.mapToElement(p, repaintRect);
		if (!mapped) {
			return false;
		} else if (!mapped->isEmpty()) {
			region += *mapped;
		}
	}
	return true;
}

} // namespace

int ServiceBoxContent::width() {
	return st::msgServiceGiftBoxSize.width();
}

ServiceBox::ServiceBox(
	not_null<Element*> parent,
	std::unique_ptr<ServiceBoxContent> content)
: Media(parent)
, _parent(parent)
, _content(std::move(content))
, _button({ .link = _content->createViewLink() })
, _maxWidth(_content->width()
	- st::msgPadding.left()
	- st::msgPadding.right())
, _title(
	st::defaultSubsectionTitle.style,
	_content->title(),
	kMarkupTextOptions,
	_maxWidth,
	Core::TextContext({
		.session = &parent->history()->session(),
		.repaint = textRepaintCallback(
			TextPart::Title,
			++_titleRepaint.generation),
	}))
, _author(
	st::uniqueGiftReleasedBy.style,
	_content->author(),
	kMarkupTextOptions,
	_maxWidth)
, _subtitle(
	st::premiumPreviewAbout.style,
	Ui::Text::Filtered(
		_content->subtitle(),
		{
			EntityType::Bold,
			EntityType::StrikeOut,
			EntityType::Underline,
			EntityType::Italic,
			EntityType::Spoiler,
			EntityType::CustomEmoji,
		}),
	kMarkupTextOptions,
	_maxWidth,
	Core::TextContext({
		.session = &parent->history()->session(),
		.repaint = textRepaintCallback(
			TextPart::Subtitle,
			++_subtitleRepaint.generation),
	}))
, _size(
	_content->width(),
	(st::msgServiceGiftBoxTopSkip
		+ _content->top()
		+ _content->size().height()
		+ st::msgServiceGiftBoxTitlePadding.top()
		+ (_title.isEmpty()
			? 0
			: (_title.countHeight(_maxWidth)
				+ st::msgServiceGiftBoxTitlePadding.bottom()))
		+ (_author.isEmpty()
			? 0
			: (st::giftBoxReleasedByMargin.top()
				+ st::uniqueGiftReleasedBy.style.font->height
				+ st::giftBoxReleasedByMargin.bottom()
				+ st::msgServiceGiftBoxTitlePadding.bottom()))
		+ _subtitle.countHeight(_maxWidth)
		+ (!_content->button()
			? 0
			: (_content->buttonSkip() + st::msgServiceGiftBoxButtonHeight))
		+ st::msgServiceGiftBoxButtonMargins.bottom()))
, _innerSize(_size - QSize(0, st::msgServiceGiftBoxTopSkip)) {
	InitElementTextPart(_parent, _subtitle);
	const auto weak = base::make_weak(this);
	_button.repaint = [weak] {
		if (const auto strong = weak.get()) {
			strong->repaintButton();
		}
	};
	if (auto text = _content->button()) {
		std::move(text) | rpl::on_next([=](QString value) {
			const auto sizeWas = _button.size;
			_button.text.setText(st::semiboldTextStyle, value);
			const auto height = st::msgServiceGiftBoxButtonHeight;
			const auto &padding = st::msgServiceGiftBoxButtonPadding;
			const auto empty = sizeWas.isEmpty();
			_button.size = QSize(
				(_button.text.maxWidth()
					+ height
					+ padding.left()
					+ padding.right()),
				height);
			if (_button.size != sizeWas) {
				invalidateButtonRepaint();
				_button.ripple = nullptr;
				if (_button.lastFg) {
					*_button.lastFg = QColor();
				}
			}
			if (!empty) {
				repaintButton();
			}
		}, _lifetime);
	}
	if (const auto type = _content->buttonMinistars()) {
		_button.stars = std::make_unique<Ui::Premium::ColoredMiniStars>(
			[weak](const QRect &) {
				if (const auto strong = weak.get()) {
					strong->repaintButton();
				}
			},
			*type);
		_button.lastFg = std::make_unique<QColor>();
	}

	if (auto changes = _content->changes()) {
		std::move(changes) | rpl::on_next([=] {
			applyContentChanges();
		}, _lifetime);
	}
}

ServiceBox::~ServiceBox() {
	++_titleRepaint.generation;
	++_subtitleRepaint.generation;
}

Fn<void()> ServiceBox::textRepaintCallback(
		TextPart part,
		uint64 generation) {
	const auto weak = base::make_weak(this);
	return [weak, part, generation] {
		if (const auto strong = weak.get()) {
			strong->repaintText(part, generation);
		}
	};
}

void ServiceBox::setSubtitle(const TextWithEntities &subtitle) {
	invalidateTextRepaint(_subtitleRepaint);
	const auto generation = ++_subtitleRepaint.generation;
	_subtitle = Ui::Text::String(
		st::premiumPreviewAbout.style,
		Ui::Text::Filtered(
			subtitle,
			{
				EntityType::Bold,
				EntityType::StrikeOut,
				EntityType::Underline,
				EntityType::Italic,
				EntityType::Spoiler,
				EntityType::CustomEmoji,
			}),
		kMarkupTextOptions,
		_maxWidth,
		Core::TextContext({
			.session = &_parent->history()->session(),
			.repaint = textRepaintCallback(
				TextPart::Subtitle,
				generation),
		}));
	InitElementTextPart(_parent, _subtitle);
}

void ServiceBox::applyContentChanges() {
	const auto subtitleWas = _subtitle.countHeight(_maxWidth);

	const auto parent = _parent;
	setSubtitle(_content->subtitle());
	const auto subtitleNow = _subtitle.countHeight(_maxWidth);
	if (subtitleNow != subtitleWas) {
		_size.setHeight(_size.height() - subtitleWas + subtitleNow);
		_innerSize = _size - QSize(0, st::msgServiceGiftBoxTopSkip);

		const auto item = parent->data();
		item->history()->owner().requestItemResize(item);
	} else {
		parent->repaint();
	}
}

QSize ServiceBox::countOptimalSize() {
	invalidateTextRepaint(_titleRepaint);
	invalidateTextRepaint(_subtitleRepaint);
	invalidateButtonRepaint();
	return _size;
}

QSize ServiceBox::countCurrentSize(int newWidth) {
	invalidateTextRepaint(_titleRepaint);
	invalidateTextRepaint(_subtitleRepaint);
	invalidateButtonRepaint();
	return _size;
}

void ServiceBox::draw(Painter &p, const PaintContext &context) const {
	p.translate(0, st::msgServiceGiftBoxTopSkip);

	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	p.setBrush(context.st->msgServiceBg());

	const auto radius = st::msgServiceGiftBoxRadius;
	if (_parent->data()->inlineReplyKeyboard()) {
		const auto r = Rect(_innerSize);
		const auto half = r.height() / 2;
		p.setClipRect(r - QMargins(0, 0, 0, half));
		p.drawRoundedRect(r, radius, radius);
		p.setClipRect(r - QMargins(0, r.height() - half, 0, 0));
		const auto small = Ui::BubbleRadiusSmall();
		p.drawRoundedRect(r, small, small);
		p.setClipping(false);
	} else {
		p.drawRoundedRect(Rect(_innerSize), radius, radius);
	}

	if (_button.stars) {
		const auto &c = context.st->msgServiceFg()->c;
		if ((*_button.lastFg) != c) {
			_button.lastFg->setRgb(c.red(), c.green(), c.blue());
			const auto padding = _button.size.height() / 2;
			_button.stars->setColorOverride(QGradientStops{
				{ 0., anim::with_alpha(c, .3) },
				{ 1., c },
			});
			_button.stars->setCenter(
				Rect(_button.size) - QMargins(padding, 0, padding, 0));
		}
	}

	const auto content = contentRect();
	auto top = content.top() + content.height();
	auto titleRepaintRegion = QRegion();
	auto titleRepaintKnown = context.hasElementPainter(p);
	{
		p.setPen(context.st->msgServiceFg());
		const auto &padding = st::msgServiceGiftBoxTitlePadding;
		top += padding.top();
		if (!_title.isEmpty()) {
			const auto titleHeight = _title.countHeight(_maxWidth);
			_parent->prepareCustomEmojiPaint(p, context, _title);
			auto customEmojiRepaintBounds
				= Ui::Text::CustomEmojiRepaintBounds();
			_title.draw(p, {
				.position = QPoint(st::msgPadding.left(), top),
				.availableWidth = _maxWidth,
				.align = style::al_top,
				.palette = &context.st->serviceTextPalette(),
				.spoiler = Ui::Text::DefaultSpoilerCache(),
				.now = context.now,
				.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
				.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
				.customEmojiRepaintBounds = &customEmojiRepaintBounds,
			});
			titleRepaintKnown = titleRepaintKnown
				&& AddTextRepaintBounds(
					titleRepaintRegion,
					p,
					context,
					_title,
					QRectF(
						st::msgPadding.left(),
						top,
						_maxWidth,
						titleHeight),
					customEmojiRepaintBounds);
			top += titleHeight + padding.bottom();
		}
		finishTextRepaint(
			_titleRepaint,
			p,
			context,
			std::move(titleRepaintRegion),
			titleRepaintKnown);
		if (!_author.isEmpty()) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(context.st->msgServiceBg());
			const auto use = std::min(_maxWidth, _author.maxWidth())
				+ st::giftBoxReleasedByMargin.left()
				+ st::giftBoxReleasedByMargin.right();
			const auto left = st::msgPadding.left() + (_maxWidth - use) / 2;
			const auto height = st::giftBoxReleasedByMargin.top()
				+ st::uniqueGiftReleasedBy.style.font->height
				+ st::giftBoxReleasedByMargin.bottom();
			const auto radius = height / 2.;
			p.drawRoundedRect(left, top, use, height, radius, radius);

			auto fg = context.st->msgServiceFg()->c;
			fg.setAlphaF(0.65 * fg.alphaF());
			p.setPen(fg);
			_author.draw(p, {
				.position = QPoint(
					left + st::giftBoxReleasedByMargin.left(),
					top + st::giftBoxReleasedByMargin.top()),
				.availableWidth = (use
					- st::giftBoxReleasedByMargin.left()
					- st::giftBoxReleasedByMargin.right()),
				.palette = &context.st->serviceTextPalette(),
				.elisionLines = 1,
			});
			p.setPen(context.st->msgServiceFg());

			top += height + st::msgServiceGiftBoxTitlePadding.bottom();
		}
		auto subtitleRepaintRegion = QRegion();
		auto subtitleRepaintKnown = context.hasElementPainter(p);
		const auto subtitleHeight = _subtitle.countHeight(_maxWidth);
		_parent->prepareCustomEmojiPaint(p, context, _subtitle);
		auto customEmojiRepaintBounds
			= Ui::Text::CustomEmojiRepaintBounds();
		_subtitle.draw(p, {
			.position = QPoint(st::msgPadding.left(), top),
			.availableWidth = _maxWidth,
			.align = style::al_top,
			.palette = &context.st->serviceTextPalette(),
			.spoiler = Ui::Text::DefaultSpoilerCache(),
			.now = context.now,
			.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
			.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
			.customEmojiRepaintBounds = &customEmojiRepaintBounds,
		});
		subtitleRepaintKnown = subtitleRepaintKnown
			&& AddTextRepaintBounds(
				subtitleRepaintRegion,
				p,
				context,
				_subtitle,
				QRectF(
					st::msgPadding.left(),
					top,
					_maxWidth,
					subtitleHeight),
				customEmojiRepaintBounds);
		top += subtitleHeight + padding.bottom();
		finishTextRepaint(
			_subtitleRepaint,
			p,
			context,
			std::move(subtitleRepaintRegion),
			subtitleRepaintKnown);
	}

	if (!_button.empty()) {
		const auto position = buttonRect().topLeft();
		p.translate(position);
		recordButtonRepaintRect(
			p,
			context,
			Rect(_button.size).marginsAdded(Margins(st::lineWidth)));

		p.setPen(Qt::NoPen);
		p.setBrush(context.st->msgServiceBg()); // ?
		if (const auto stars = _button.stars.get()) {
			stars->setPaused(context.paused);
		}
		_button.drawBg(p);
		p.setPen(context.st->msgServiceFg());
		if (_button.ripple) {
			const auto opacity = p.opacity();
			p.setOpacity(st::historyPollRippleOpacity);
			_button.ripple->paint(
				p,
				0,
				0,
				_button.size.width(),
				&context.messageStyle()->msgWaveformInactive->c);
			p.setOpacity(opacity);
		}
		_button.text.draw(
			p,
			0,
			(_button.size.height() - _button.text.minHeight()) / 2,
			_button.size.width(),
			style::al_top);

		p.translate(-position);
	} else {
		recordButtonRepaintRect(p, context, QRect());
	}

	_content->draw(p, context, content);

	if (const auto tag = _content->cornerTag(context); !tag.isNull()) {
		const auto width = tag.width() / tag.devicePixelRatio();
		p.drawImage(_innerSize.width() - width, 0, tag);
	}

	p.translate(0, -st::msgServiceGiftBoxTopSkip);
}

TextState ServiceBox::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	point.setY(point.y() - st::msgServiceGiftBoxTopSkip);
	const auto content = contentRect();
	const auto lookupSubtitleLink = [&] {
		auto top = content.top() + content.height();
		const auto &padding = st::msgServiceGiftBoxTitlePadding;
		top += padding.top();
		if (!_title.isEmpty()) {
			top += _title.countHeight(_maxWidth) + padding.bottom();
		}
		if (!_author.isEmpty()) {
			const auto use = std::min(_maxWidth, _author.maxWidth())
				+ st::giftBoxReleasedByMargin.left()
				+ st::giftBoxReleasedByMargin.right();
			const auto left = st::msgPadding.left() + (_maxWidth - use) / 2;
			const auto height = st::giftBoxReleasedByMargin.top()
				+ st::defaultTextStyle.font->height
				+ st::giftBoxReleasedByMargin.bottom();
			if (point.x() >= left
				&& point.y() >= top
				&& point.x() < left + use
				&& point.y() < top + height) {
				result.link = _content->authorLink();
			}
			top += height + st::msgServiceGiftBoxTitlePadding.bottom();
		}

		auto subtitleRequest = request.forText();
		subtitleRequest.align = style::al_top;
		const auto state = _subtitle.getState(
			point - QPoint(st::msgPadding.left(), top),
			_maxWidth,
			subtitleRequest);
		if (state.link) {
			result.link = state.link;
		}
	};
	if (_button.empty()) {
		if (!_button.link) {
			lookupSubtitleLink();
		} else if (QRect(QPoint(), _innerSize).contains(point)) {
			result.link = _button.link;
		}
	} else {
		const auto rect = buttonRect();
		if (rect.contains(point)) {
			result.link = _button.link;
			_button.lastPoint = point - rect.topLeft();
		} else if (content.contains(point)) {
			if (!_contentLink) {
				_contentLink = _content->createViewLink();
			}
			result.link = _contentLink;
		} else {
			lookupSubtitleLink();
		}
	}
	return result;
}

bool ServiceBox::toggleSelectionByHandlerClick(
		const ClickHandlerPtr &p) const {
	return false;
}

bool ServiceBox::dragItemByHandler(const ClickHandlerPtr &p) const {
	return false;
}

void ServiceBox::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (!handler) {
		return;
	}

	if (handler == _button.link) {
		_button.toggleRipple(pressed);
	}
}

void ServiceBox::stickerClearLoopPlayed() {
	_content->stickerClearLoopPlayed();
}

std::unique_ptr<StickerPlayer> ServiceBox::stickerTakePlayer(
		not_null<DocumentData*> data,
		const Lottie::ColorReplacements *replacements) {
	return _content->stickerTakePlayer(data, replacements);
}

bool ServiceBox::needsBubble() const {
	return false;
}

bool ServiceBox::customInfoLayout() const {
	return false;
}

void ServiceBox::hideSpoilers() {
	_subtitle.setSpoilerRevealed(false, anim::type::instant);
}

bool ServiceBox::hasHeavyPart() const {
	return _content->hasHeavyPart();
}

void ServiceBox::unloadHeavyPart() {
	_content->unloadHeavyPart();
	_title.unloadPersistentAnimation();
	_subtitle.unloadPersistentAnimation();
}

QRect ServiceBox::buttonRect() const {
	const auto &padding = st::msgServiceGiftBoxButtonMargins;
	const auto position = QPoint(
		(width() - _button.size.width()) / 2,
		height() - padding.bottom() - _button.size.height());
	return QRect(position, _button.size);
}

QRect ServiceBox::contentRect() const {
	const auto size = _content->size();
	const auto top = _content->top();
	return QRect(QPoint((width() - size.width()) / 2, top), size);
}

void ServiceBox::repaintText(TextPart part, uint64 generation) const {
	auto &repaint = (part == TextPart::Title)
		? _titleRepaint
		: _subtitleRepaint;
	if (generation != repaint.generation || repaint.pending) {
		return;
	} else if (repaint.known && repaint.current.isEmpty()) {
		return;
	}
	repaint.pending = true;
	if (!repaint.known) {
		Ui::LogUnknownGeometryRepaint("service box text");
		_parent->customEmojiRepaint();
	} else {
		repaintTextRegion(repaint.current);
	}
}

void ServiceBox::finishTextRepaint(
		TextRepaint &repaint,
		const Painter &p,
		const PaintContext &context,
		QRegion region,
		bool geometryKnown) const {
	if (!context.hasElementPainter(p)) {
		if (!repaint.known) {
			repaint.pending = false;
		}
		return;
	}
	repaint.pending = false;
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.current = geometryKnown ? std::move(region) : QRegion();
	repaint.known = geometryKnown;
	if (!geometryKnown) {
		repaint.stale = previous;
		if (stale.isEmpty() && !previous.isEmpty()) {
			repaint.pending = true;
			repaintTextRegion(previous);
		}
		return;
	}
	if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = true;
	repaintTextRegion(previous.united(repaint.current));
}

void ServiceBox::invalidateTextRepaint(TextRepaint &repaint) const {
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.known = false;
}

void ServiceBox::repaintTextRegion(const QRegion &region) const {
	_parent->repaint(region);
}

void ServiceBox::repaintButton() const {
	auto &repaint = _button.animationRepaint;
	if (repaint.pending
		|| (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = true;
	if (!repaint.known) {
		Ui::LogUnknownGeometryRepaint("service box button");
		this->repaint();
	} else {
		repaintButtonRegion(repaint.current);
	}
}

void ServiceBox::recordButtonRepaintRect(
		const Painter &p,
		const PaintContext &context,
		QRect rect) const {
	if (!context.hasElementPainter(p)) {
		return;
	}
	auto current = QRegion();
	auto known = true;
	if (!rect.isEmpty()) {
		const auto mapped = context.mapToElement(p, QRectF(rect));
		if (!mapped) {
			known = false;
		} else if (!mapped->isEmpty()) {
			current = QRegion(*mapped);
		}
	}
	auto &repaint = _button.animationRepaint;
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.current = known ? std::move(current) : QRegion();
	repaint.known = known;
	if (!known) {
		repaint.stale = previous;
		if (!previous.isEmpty()) {
			repaint.pending = true;
			Ui::LogUnknownGeometryRepaint("service box button");
			this->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = true;
	repaintButtonRegion(previous.united(repaint.current));
}

void ServiceBox::invalidateButtonRepaint() const {
	auto &repaint = _button.animationRepaint;
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.known = false;
}

void ServiceBox::repaintButtonRegion(const QRegion &region) const {
	_parent->repaint(region);
}

void ServiceBox::Button::toggleRipple(bool pressed) {
	if (empty()) {
		return;
	} else if (pressed) {
		const auto linkWidth = size.width();
		const auto linkHeight = size.height();
		if (!ripple) {
			const auto drawMask = [&](QPainter &p) { drawBg(p); };
			auto mask = Ui::RippleAnimation::MaskByDrawer(
				QSize(linkWidth, linkHeight),
				false,
				drawMask);
			ripple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				std::move(mask),
				repaint);
		}
		ripple->add(lastPoint);
	} else if (ripple) {
		ripple->lastStop();
	}
}

bool ServiceBox::Button::empty() const {
	return text.isEmpty();
}

void ServiceBox::Button::drawBg(QPainter &p) const {
	const auto radius = size.height() / 2.;
	const auto r = Rect(size);
	p.drawRoundedRect(r, radius, radius);
	if (stars) {
		auto clipPath = QPainterPath();
		clipPath.addRoundedRect(r, radius, radius);
		p.setClipPath(clipPath);
		stars->paint(p);
		p.setClipping(false);
	}
}

} // namespace HistoryView
