/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_view_button.h"

#include "boxes/gift_premium_box.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item_components.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "iv/iv_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"

namespace HistoryView {
namespace {

[[nodiscard]] ClickHandlerPtr MakeMediaButtonClickHandler(
		not_null<Data::Media*> media) {
	const auto start = media->giveawayStart();
	const auto results = media->giveawayResults();
	Assert(start || results);

	const auto peer = media->parent()->history()->peer;
	const auto messageId = media->parent()->id;
	if (media->parent()->isSending() || media->parent()->hasFailed()) {
		return nullptr;
	}
	const auto maybeStart = start
		? *start
		: std::optional<Data::GiveawayStart>();
	const auto maybeResults = results
		? *results
		: std::optional<Data::GiveawayResults>();
	return std::make_shared<LambdaClickHandler>([=](
			ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		const auto controller = my.sessionWindow.get();
		if (!controller) {
			return;
		}
		ResolveGiveawayInfo(
			controller,
			peer,
			messageId,
			maybeStart,
			maybeResults);
	});
}

[[nodiscard]] QString MakeMediaButtonText(not_null<Data::Media*> media) {
	Expects(media->giveawayStart() || media->giveawayResults());

	return tr::lng_prizes_how_works(tr::now, tr::upper);
}

[[nodiscard]] ClickHandlerPtr MakeRichMessageButtonClickHandler(
		FullMsgId itemId) {
	return std::make_shared<LambdaClickHandler>([=](ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		const auto controller = my.sessionWindow.get();
		const auto item = controller
			? controller->session().data().message(itemId)
			: nullptr;
		if (!controller || !item) {
			return;
		}
		Core::App().iv().showRichMessage(controller, not_null{ item });
	});
}

[[nodiscard]] QString MakeRichMessageButtonText() {
	return tr::lng_view_button_full_article(tr::now);
}

} // namespace

struct ViewButton::Inner {
	Inner(
		not_null<Data::Media*> media,
		uint8 colorIndex,
		not_null<const Element*> owner);
	Inner(
		FullMsgId itemId,
		uint8 colorIndex,
		not_null<const Element*> owner);

	void createRipple(int height);
	void toggleRipple(bool pressed, int height);

	const Kind kind;
	const style::margins &margins;
	const ClickHandlerPtr link;
	const not_null<const Element*> owner;
	Data::Media *media = nullptr;
	FullMsgId itemId;
	uint32 lastWidth : 24 = 0;
	uint32 colorIndex : 6 = 0;
	uint32 aboveInfo : 1 = 0;
	uint32 externalLink : 1 = 0;
	QPoint lastPoint;
	PaintedRectRepaintTracker rippleRepaint;
	std::unique_ptr<Ui::RippleAnimation> ripple;
	Ui::Text::String text;
};

bool ViewButton::MediaHasViewButton(not_null<Data::Media*> media) {
	return media->giveawayStart() || media->giveawayResults();
}

ViewButton::Inner::Inner(
	not_null<Data::Media*> media,
	uint8 colorIndex,
	not_null<const Element*> owner)
: kind(Kind::Giveaway)
, margins(st::historyViewButtonMargins)
, link(MakeMediaButtonClickHandler(media))
, owner(owner)
, media(media)
, colorIndex(colorIndex)
, aboveInfo(1)
, text(st::historyViewButtonTextStyle, MakeMediaButtonText(media)) {
}

ViewButton::Inner::Inner(
	FullMsgId itemId,
	uint8 colorIndex,
	not_null<const Element*> owner)
: kind(Kind::RichMessage)
, margins(st::historyViewButtonMargins)
, link(MakeRichMessageButtonClickHandler(itemId))
, owner(owner)
, itemId(itemId)
, colorIndex(colorIndex)
, aboveInfo(1)
, text(st::historyViewButtonTextStyle, MakeRichMessageButtonText()) {
}

void ViewButton::Inner::createRipple(int height) {
	const auto radius = (kind == Kind::RichMessage)
		? st::historyPagePreview.radius
		: st::roundRadiusLarge;
	ripple = std::make_unique<Ui::RippleAnimation>(
		st::defaultRippleAnimation,
		Ui::RippleAnimation::RoundRectMask(
			QSize(lastWidth, height - margins.top() - margins.bottom()),
			radius),
		[this] {
			rippleRepaint.request(owner, "message view button ripple");
		});
}

void ViewButton::Inner::toggleRipple(bool pressed, int height) {
	if (pressed) {
		if (!ripple) {
			createRipple(height);
		}
		ripple->add(lastPoint);
	} else if (ripple) {
		ripple->lastStop();
	}
}

ViewButton::ViewButton(
	not_null<Data::Media*> media,
	uint8 colorIndex,
	not_null<const Element*> owner)
: _inner(std::make_unique<Inner>(
	media,
	colorIndex,
	owner)) {
}

ViewButton::ViewButton(
		FullMsgId itemId,
		uint8 colorIndex,
		not_null<const Element*> owner)
: _inner(std::make_unique<Inner>(
	itemId,
	colorIndex,
	owner)) {
}

ViewButton::~ViewButton() {
}

bool ViewButton::matches(not_null<Data::Media*> media) const {
	return (_inner->kind == Kind::Giveaway) && (_inner->media == media);
}

bool ViewButton::matches(FullMsgId itemId) const {
	return (_inner->kind == Kind::RichMessage) && (_inner->itemId == itemId);
}

void ViewButton::resized() const {
	if (_inner->ripple) {
		_inner->createRipple(height());
	}
}

int ViewButton::height() const {
	return (_inner->kind == Kind::RichMessage)
		? (st::historyPageButtonHeight
			+ _inner->margins.top()
			+ _inner->margins.bottom())
		: st::historyViewButtonHeight;
}

bool ViewButton::belowMessageInfo() const {
	return !_inner->aboveInfo;
}

void ViewButton::draw(
		Painter &p,
		const QRect &r,
		const Ui::ChatPaintContext &context) {
	const auto canonical = context.hasElementPainter(p);
	const auto hadRipple = (_inner->ripple != nullptr);
	const auto widthChanged = canonical && (_inner->lastWidth != r.width());
	const auto st = context.st;
	const auto stm = context.messageStyle();
	const auto selected = context.selected();
	const auto cache = context.outbg
		? stm->replyCache[st->colorPatternIndex(_inner->colorIndex)].get()
		: st->coloredReplyCache(selected, _inner->colorIndex).get();
	auto rippleRect = style::rtlrect(
		r.left(),
		r.top(),
		r.width(),
		r.height(),
		r.width());
	const auto paintRipple = [&] {
		if (!_inner->ripple) {
			return;
		}
		const auto painted = _inner->ripple->paint(
			p,
			r.left(),
			r.top(),
			r.width(),
			&cache->bg);
		if (!painted.isEmpty()) {
			rippleRect = painted;
		}
		if (canonical && _inner->ripple->empty()) {
			_inner->ripple.reset();
		}
	};
	if (_inner->kind == Kind::RichMessage) {
		Ui::Text::ValidateQuotePaintCache(*cache, st::historyPagePreview);
		Ui::Text::FillQuotePaint(p, r, *cache, st::historyPagePreview);
		paintRipple();
		const auto padding = st::historyPageButtonPadding;
		const auto availableWidth = r.width()
			- padding.left()
			- padding.right();
		if (availableWidth > 0) {
			const auto textWidth = (_inner->text.maxWidth() < availableWidth)
				? _inner->text.maxWidth()
				: availableWidth;
			p.setPen(cache->icon);
			_inner->text.drawElided(
				p,
				r.left()
					+ padding.left()
					+ (availableWidth - textWidth) / 2,
				r.top() + padding.top(),
				textWidth,
				1,
				style::al_top);
		}
	} else {
		const auto radius = st::historyPagePreview.radius;
		paintRipple();
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(cache->bg);
		p.drawRoundedRect(r, radius, radius);

		p.setPen(cache->icon);
		_inner->text.drawElided(
			p,
			r.left(),
			r.top() + (r.height() - _inner->text.minHeight()) / 2,
			r.width(),
			1,
			style::al_top);

		if (_inner->externalLink) {
			const auto &icon = st::msgBotKbUrlIcon;
			const auto padding = st::msgBotKbIconPadding;
			icon.paint(
				p,
				r.left() + r.width() - icon.width() - padding,
				r.top() + padding,
				r.width(),
				cache->icon);
		}
	}
	_inner->rippleRepaint.record(
		_inner->owner,
		canonical,
		canonical
			? context.mapToElement(p, QRectF(rippleRect))
			: std::optional<QRect>(),
		!hadRipple,
		"message view button ripple");
	if (widthChanged) {
		_inner->lastWidth = r.width();
		if (_inner->ripple) {
			_inner->rippleRepaint.repaintBeforeRemoval(
				_inner->owner,
				"message view button ripple");
		}
		resized();
	}
}

void ViewButton::recordRipplePaintAbsent(bool canonical) const {
	_inner->rippleRepaint.record(
		_inner->owner,
		canonical,
		QRect(),
		!_inner->ripple,
		"message view button ripple");
}

void ViewButton::repaintBeforeRemoval() const {
	if (_inner->ripple) {
		_inner->rippleRepaint.repaintBeforeRemoval(
			_inner->owner,
			"message view button ripple");
	}
}

const ClickHandlerPtr &ViewButton::link() const {
	return _inner->link;
}

bool ViewButton::checkLink(const ClickHandlerPtr &other, bool pressed) {
	if (_inner->link != other) {
		return false;
	}
	_inner->toggleRipple(pressed, height());
	return true;
}

bool ViewButton::getState(
		QPoint point,
		const QRect &g,
		not_null<TextState*> outResult) const {
	if (!g.contains(point)) {
		return false;
	}
	outResult->link = _inner->link;
	if (!_inner->ripple) {
		_inner->lastWidth = g.width();
	}
	_inner->lastPoint = point - g.topLeft();
	return true;
}

QRect ViewButton::countRect(const QRect &r) const {
	return QRect(
		r.left(),
		r.top() + r.height() - height(),
		r.width(),
		height()) - _inner->margins;
}

} // namespace HistoryView
