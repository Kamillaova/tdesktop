/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_media_generic.h"

#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "ui/chat/chat_style.h"
#include "ui/dynamic_image.h"
#include "ui/dynamic_thumbnails.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/rect.h"
#include "ui/round_rect.h"
#include "ui/userpic_view.h"
#include "styles/style_chat.h"

namespace HistoryView {
namespace {

constexpr auto kAdditionalPrizesWithLineOpacity = 0.6;

} // namespace

TextState MediaGenericPart::textState(
		QPoint point,
		StateRequest request,
		int outerWidth) const {
	return {};
}

void MediaGenericPart::clickHandlerPressedChanged(
	const ClickHandlerPtr &p,
	bool pressed) {
}

bool MediaGenericPart::hasHeavyPart() {
	return false;
}

void MediaGenericPart::unloadHeavyPart() {
}

auto MediaGenericPart::stickerTakePlayer(
	not_null<DocumentData*> data,
	const Lottie::ColorReplacements *replacements
) -> std::unique_ptr<StickerPlayer> {
	return nullptr;
}

uint16 MediaGenericPart::fullSelectionLength() const {
	return 0;
}

TextSelection MediaGenericPart::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	return selection;
}

TextForMimeData MediaGenericPart::selectedText(
		TextSelection selection) const {
	return {};
}

void MediaGenericPart::requestAnimationRepaint(
		not_null<Element*> parent,
		AnimationRepaint &repaint) const {
	if (repaint.pending
		|| (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = 1;
	if (!repaint.known) {
		parent->repaint();
		return;
	}
	parent->repaint(repaint.current);
}

void MediaGenericPart::recordAnimationRepaint(
		not_null<Element*> parent,
		const Painter &p,
		const PaintContext &context,
		AnimationRepaint &repaint,
		QRectF rect,
		bool known) const {
	if (!context.hasElementPainter(p)) {
		return;
	}
	auto current = QRect();
	if (known && !rect.isEmpty()) {
		const auto mapped = context.mapToElement(p, rect);
		if (!mapped || mapped->isEmpty()) {
			known = false;
		} else {
			current = *mapped;
		}
	}
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.pending = 0;
	repaint.current = known ? current : QRect();
	repaint.known = known ? 1 : 0;
	if (!known) {
		repaint.stale = previous;
		if (stale.isEmpty() && !previous.isEmpty()) {
			repaint.pending = 1;
			parent->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = 1;
	parent->repaint(previous.united(repaint.current));
}

void MediaGenericPart::invalidateAnimationRepaint(
		AnimationRepaint &repaint) const {
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = 0;
	repaint.known = 0;
}

void MediaGenericPart::resetAnimationRepaint(
		AnimationRepaint &repaint) const {
	repaint = {};
}

MediaGeneric::MediaGeneric(
	not_null<Element*> parent,
	Fn<void(
		not_null<MediaGeneric*>,
		Fn<void(std::unique_ptr<Part>)>)> generate,
	MediaGenericDescriptor &&descriptor)
: Media(parent)
, _paintBgFactory(std::move(descriptor.paintBgFactory))
, _paintBg(_paintBgFactory ? _paintBgFactory() : nullptr)
, _fullAreaLink(descriptor.fullAreaLink)
, _maxWidthCap(descriptor.maxWidth)
, _expandCurrentWidth(descriptor.expandCurrentWidth)
, _service(descriptor.service)
, _hideServiceText(descriptor.hideServiceText) {
	generate(this, [&](std::unique_ptr<Part> part) {
		_entries.push_back({
			.object = std::move(part),
		});
	});
}

MediaGeneric::~MediaGeneric() {
	if (hasHeavyPart()) {
		unloadHeavyPart();
		_parent->checkHeavyPart();
	}
}

QSize MediaGeneric::countOptimalSize() {
	const auto maxWidth = _maxWidthCap
		? _maxWidthCap
		: st::chatGiveawayWidth;

	auto top = 0;
	for (auto &entry : _entries) {
		const auto raw = entry.object.get();
		raw->initDimensions();
		top += raw->resizeGetHeight(maxWidth);
	}
	return { maxWidth, top };
}

QSize MediaGeneric::countCurrentSize(int newWidth) {
	if (!_expandCurrentWidth && newWidth > maxWidth()) {
		newWidth = maxWidth();
	}
	auto top = 0;
	for (auto &entry : _entries) {
		top += entry.object->resizeGetHeight(newWidth);
	}
	return { newWidth, top };
}

void MediaGeneric::draw(Painter &p, const PaintContext &context) const {
	const auto outer = width();
	if (outer < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return;
	}
	if (!_paintBg && _paintBgFactory) {
		_paintBg = _paintBgFactory();
	}
	if (_paintBg) {
		_paintBg(p, context, this);
	} else if (_service) {
		PainterHighQualityEnabler hq(p);
		const auto radius = st::msgServiceGiftBoxRadius;
		p.setPen(Qt::NoPen);
		p.setBrush(context.st->msgServiceBg());
		const auto rect = QRect(0, 0, width(), height());
		if (parent()->data()->inlineReplyKeyboard()) {
			const auto half = rect.height() / 2;
			p.setClipRect(rect - QMargins(0, 0, 0, half));
			p.drawRoundedRect(rect, radius, radius);
			p.setClipRect(rect - QMargins(0, rect.height() - half, 0, 0));
			const auto small = Ui::BubbleRadiusSmall();
			p.drawRoundedRect(rect, small, small);
			p.setClipping(false);
		} else {
			p.drawRoundedRect(rect, radius, radius);
		}
	}

	const auto fullSelection = context.selected();
	auto translated = 0;
	auto symbolOffset = uint16(0);
	for (const auto &entry : _entries) {
		const auto raw = entry.object.get();
		const auto height = raw->height();
		const auto length = raw->fullSelectionLength();
		if (length > 0 && !fullSelection) {
			const auto local = UnshiftItemSelection(
				context.selection,
				symbolOffset);
			raw->draw(p, this, context.withSelection(local), outer);
		} else {
			raw->draw(p, this, context, outer);
		}
		translated += height;
		symbolOffset = uint16(symbolOffset + length);
		p.translate(0, height);
	}
	p.translate(0, -translated);
}

TextState MediaGeneric::textState(
		QPoint point,
		StateRequest request) const {
	auto result = TextState(_parent);

	const auto outer = width();
	if (outer < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return result;
	}

	if (_fullAreaLink && QRect(0, 0, width(), height()).contains(point)) {
		result.link = _fullAreaLink;
		return result;
	}

	auto symbolOffset = uint16(0);
	for (const auto &entry : _entries) {
		const auto raw = entry.object.get();
		const auto height = raw->height();
		const auto length = raw->fullSelectionLength();
		if (point.y() >= 0 && point.y() < height) {
			const auto part = raw->textState(point, request, outer);
			result.link = part.link;
			result.cursor = part.cursor;
			if (length > 0) {
				result.symbol = uint16(symbolOffset + part.symbol);
				result.afterSymbol = part.afterSymbol;
				result.overMessageText
					= (part.cursor == CursorState::Text);
			} else {
				result.symbol = symbolOffset;
			}
			return result;
		}
		point.setY(point.y() - height);
		symbolOffset = uint16(symbolOffset + length);
	}
	result.symbol = symbolOffset;
	return result;
}

void MediaGeneric::clickHandlerActiveChanged(
		const ClickHandlerPtr &p,
		bool active) {
}

void MediaGeneric::clickHandlerPressedChanged(
		const ClickHandlerPtr &p,
		bool pressed) {
	for (const auto &entry : _entries) {
		entry.object->clickHandlerPressedChanged(p, pressed);
	}
}

bool MediaGeneric::hasTextForCopy() const {
	return fullSelectionLength() > 0;
}

uint16 MediaGeneric::fullSelectionLength() const {
	auto total = uint16(0);
	for (const auto &entry : _entries) {
		total = uint16(total + entry.object->fullSelectionLength());
	}
	return total;
}

TextForMimeData MediaGeneric::selectedText(TextSelection selection) const {
	auto offset = uint16(0);
	auto result = TextForMimeData();
	for (const auto &entry : _entries) {
		const auto length = entry.object->fullSelectionLength();
		if (length > 0) {
			auto part = entry.object->selectedText(
				UnshiftItemSelection(selection, offset));
			if (!part.empty()) {
				if (result.empty()) {
					result = std::move(part);
				} else {
					result.append('\n').append(std::move(part));
				}
			}
		}
		offset = uint16(offset + length);
	}
	return result;
}

TextSelection MediaGeneric::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	if (selection == FullSelection) {
		return selection;
	}
	auto offset = uint16(0);
	auto firstFrom = std::optional<uint16>();
	auto firstOffset = uint16(0);
	auto lastTo = uint16(0);
	auto lastOffset = uint16(0);
	for (const auto &entry : _entries) {
		const auto length = entry.object->fullSelectionLength();
		if (length > 0) {
			const auto end = uint16(offset + length);
			if (selection.from < end && selection.to > offset) {
				const auto from = uint16((selection.from > offset)
					? (selection.from - offset)
					: 0);
				const auto to = uint16((selection.to < end)
					? (selection.to - offset)
					: length);
				const auto local = entry.object->adjustSelection(
					{ from, to },
					type);
				if (!firstFrom.has_value()) {
					firstFrom = local.from;
					firstOffset = offset;
				}
				lastTo = local.to;
				lastOffset = offset;
			}
		}
		offset = uint16(offset + length);
	}
	if (!firstFrom.has_value()) {
		return selection;
	}
	return {
		uint16(firstOffset + *firstFrom),
		uint16(lastOffset + lastTo),
	};
}

std::unique_ptr<StickerPlayer> MediaGeneric::stickerTakePlayer(
		not_null<DocumentData*> data,
		const Lottie::ColorReplacements *replacements) {
	for (const auto &entry : _entries) {
		if (auto result = entry.object->stickerTakePlayer(
				data,
				replacements)) {
			return result;
		}
	}
	return nullptr;
}

bool MediaGeneric::hideFromName() const {
	return !parent()->data()->Has<HistoryMessageForwarded>();
}

bool MediaGeneric::hideServiceText() const {
	return _hideServiceText;
}

bool MediaGeneric::hasHeavyPart() const {
	for (const auto &entry : _entries) {
		if (entry.object->hasHeavyPart()) {
			return true;
		}
	}
	return false;
}

void MediaGeneric::unloadHeavyPart() {
	_paintBg = nullptr;
	for (const auto &entry : _entries) {
		entry.object->unloadHeavyPart();
	}
}

QMargins MediaGeneric::inBubblePadding() const {
	auto lshift = st::msgPadding.left();
	auto rshift = st::msgPadding.right();
	auto bshift = isBubbleBottom()
		? st::msgPadding.top()
		: st::mediaInBubbleSkip;
	auto tshift = isBubbleTop()
		? st::msgPadding.bottom()
		: st::mediaInBubbleSkip;
	return QMargins(lshift, tshift, rshift, bshift);
}

MediaGenericTextPart::MediaGenericTextPart(
	TextWithEntities text,
	QMargins margins,
	const style::TextStyle &st,
	const base::flat_map<uint16, ClickHandlerPtr> &links,
	const Ui::Text::MarkedContext &context,
	style::align align,
	Element *repaintParent)
: _repaintParent(repaintParent)
, _text(st::msgMinWidth)
, _margins(margins)
, _align(align) {
	auto adjusted = context;
	if (_repaintParent) {
		adjusted.repaint = [this] {
			requestAnimationRepaint(
				not_null(_repaintParent),
				_textRepaint);
		};
	}
	_text.setMarkedText(
		st,
		text,
		kMarkupTextOptions,
		adjusted);
	_customEmoji = _text.hasCustomEmoji();
	_spoilers = _text.hasSpoilers();
	_animated = _customEmoji || _spoilers;
	for (const auto &[index, link] : links) {
		_text.setLink(index, link);
	}
}

void MediaGenericTextPart::draw(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth) const {
	const auto use = (width() - _margins.left() - _margins.right());
	setupPen(p, owner, context);
	const auto position = QPoint(
		(_align == style::al_top)
			? ((outerWidth - use) / 2)
			: _margins.left(),
		_margins.top());
	auto customEmojiBounds = Ui::Text::CustomEmojiPaintedBounds();
	_text.draw(p, {
		.position = position,
		.outerWidth = outerWidth,
		.availableWidth = use,
		.align = _align,
		.palette = &(owner->service()
			? context.st->serviceTextPalette()
			: context.messageStyle()->textPalette),
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
		.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
		.selection = context.selection,
		.elisionLines = elisionLines(),
		.customEmojiPaintedBounds = (_repaintParent && _customEmoji)
			? &customEmojiBounds
			: nullptr,
	});
	if (_repaintParent && _animated) {
		const auto textRect = QRectF(
			position,
			QSize(use, height() - _margins.top() - _margins.bottom()));
		auto rect = customEmojiBounds.repaintRect();
		const auto known = !_customEmoji
			|| customEmojiBounds.repaintRectKnown();
		if (_spoilers) {
			rect = rect.isEmpty() ? textRect : rect.united(textRect);
		}
		recordAnimationRepaint(
			not_null(_repaintParent),
			p,
			context,
			_textRepaint,
			rect,
			known);
	}
}

void MediaGenericTextPart::setupPen(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context) const {
	const auto service = owner->service();
	p.setPen(service
		? context.st->msgServiceFg()
		: context.messageStyle()->historyTextFg);
}

int MediaGenericTextPart::elisionLines() const {
	return 0;
}

TextState MediaGenericTextPart::textState(
		QPoint point,
		StateRequest request,
		int outerWidth) const {
	const auto use = (width() - _margins.left() - _margins.right());
	point -= QPoint{
		((_align == style::al_top)
			? ((outerWidth - use) / 2)
			: _margins.left()),
		_margins.top(),
	};
	auto forText = request.forText();
	forText.align = _align;
	return TextState(nullptr, _text.getState(point, use, forText));
}

uint16 MediaGenericTextPart::fullSelectionLength() const {
	return _text.length();
}

TextSelection MediaGenericTextPart::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	return _text.adjustSelection(selection, type);
}

TextForMimeData MediaGenericTextPart::selectedText(
		TextSelection selection) const {
	return _text.toTextForMimeData(selection);
}

QSize MediaGenericTextPart::countOptimalSize() {
	const auto lines = elisionLines();
	const auto height = lines
		? std::min(_text.minHeight(), lines * _text.style()->font->height)
		: _text.minHeight();
	return {
		_margins.left() + _text.maxWidth() + _margins.right(),
		_margins.top() + height + _margins.bottom(),
	};
}

QSize MediaGenericTextPart::countCurrentSize(int newWidth) {
	auto skip = _margins.left() + _margins.right();
	const auto size = (_align == style::al_top)
		? Ui::Text::CountOptimalTextSize(
			_text,
			st::msgMinWidth,
			std::max(st::msgMinWidth, newWidth - skip))
		: QSize(newWidth - skip, _text.countHeight(newWidth - skip));
	const auto lines = elisionLines();
	const auto height = lines
		? std::min(size.height(), lines * _text.style()->font->height)
		: size.height();
	return {
		size.width() + skip,
		_margins.top() + height + _margins.bottom(),
	};
}

TextDelimeterPart::TextDelimeterPart(
	const QString &text,
	QMargins margins)
: _margins(margins) {
	_text.setText(st::defaultTextStyle, text);
}

void TextDelimeterPart::draw(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth) const {
	const auto stm = context.messageStyle();
	const auto available = outerWidth - _margins.left() - _margins.right();
	p.setPen(stm->msgDateFg);
	_text.draw(p, {
		.position = { _margins.left(), _margins.top() },
		.outerWidth = outerWidth,
		.availableWidth = available,
		.align = style::al_top,
		.palette = &stm->textPalette,
		.now = context.now,
		.elisionLines = 1,
	});
	const auto skip = st::chatGiveawayPrizesWithSkip;
	const auto inner = available - 2 * skip;
	const auto sub = _text.maxWidth();
	if (inner > sub + 1) {
		const auto fill = (inner - sub) / 2;
		const auto stroke = st::lineWidth;
		const auto top = _margins.top()
			+ st::chatGiveawayPrizesWithLineTop;
		p.setOpacity(kAdditionalPrizesWithLineOpacity);
		p.fillRect(_margins.left(), top, fill, stroke, stm->msgDateFg);
		const auto start = outerWidth - _margins.right() - fill;
		p.fillRect(start, top, fill, stroke, stm->msgDateFg);
		p.setOpacity(1.);
	}
}

QSize TextDelimeterPart::countOptimalSize() {
	return {
		_margins.left() + _text.maxWidth() + _margins.right(),
		_margins.top() + st::normalFont->height + _margins.bottom(),
	};
}

QSize TextDelimeterPart::countCurrentSize(int newWidth) {
	return { newWidth, minHeight() };
}

LambdaGenericPart::LambdaGenericPart(
	QSize size,
	Fn<void(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth)> draw)
: _size(size)
, _draw(std::move(draw)) {
}

void LambdaGenericPart::draw(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth) const {
	if (_draw) {
		_draw(p, owner, context, outerWidth);
	}
}

QSize LambdaGenericPart::countOptimalSize() {
	return _size;
}

QSize LambdaGenericPart::countCurrentSize(int newWidth) {
	return { newWidth, _size.height() };
}

StickerInBubblePart::StickerInBubblePart(
	not_null<Element*> parent,
	Element *replacing,
	Fn<Data()> lookup,
	QMargins padding)
: _parent(parent)
, _lookup(std::move(lookup))
, _padding(padding) {
	ensureCreated(replacing);
}

void StickerInBubblePart::draw(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth) const {
	ensureCreated();
	if (_sticker) {
		const auto stickerSize = _sticker->countOptimalSize();
		const auto sticker = QRect(
			(outerWidth - stickerSize.width()) / 2,
			_padding.top() + _skipTop,
			stickerSize.width(),
			stickerSize.height());
		_sticker->draw(p, context, sticker);
	}
}

TextState StickerInBubblePart::textState(
		QPoint point,
		StateRequest request,
		int outerWidth) const {
	auto result = TextState(_parent);
	if (_sticker) {
		const auto stickerSize = _sticker->countOptimalSize();
		const auto sticker = QRect(
			(outerWidth - stickerSize.width()) / 2,
			_padding.top() + _skipTop,
			stickerSize.width(),
			stickerSize.height());
		if (sticker.contains(point)) {
			result.link = _link;
		}
	}
	return result;
}

bool StickerInBubblePart::hasHeavyPart() {
	return _sticker && _sticker->hasHeavyPart();
}

void StickerInBubblePart::unloadHeavyPart() {
	if (_sticker) {
		_sticker->unloadHeavyPart();
	}
}

std::unique_ptr<StickerPlayer> StickerInBubblePart::stickerTakePlayer(
		not_null<DocumentData*> data,
		const Lottie::ColorReplacements *replacements) {
	return _sticker
		? _sticker->stickerTakePlayer(data, replacements)
		: nullptr;
}

QSize StickerInBubblePart::countOptimalSize() {
	ensureCreated();
	const auto size = _sticker ? _sticker->countOptimalSize() : [&] {
		const auto fallback = _lookup().size;
		return QSize{ fallback, fallback };
	}();
	return {
		_padding.left() + size.width() + _padding.right(),
		_padding.top() + size.height() + _padding.bottom(),
	};
}

QSize StickerInBubblePart::countCurrentSize(int newWidth) {
	return { newWidth, minHeight() };
}

void StickerInBubblePart::ensureCreated(Element *replacing) const {
	if (_sticker) {
		return;
	} else if (const auto data = _lookup()) {
		const auto sticker = data.sticker;
		if (sticker->sticker()) {
			const auto skipPremiumEffect = true;
			_link = data.link;
			_skipTop = data.skipTop;
			_sticker.emplace(_parent, sticker, skipPremiumEffect, replacing);
			if (data.stopOnLastFrame) {
				_sticker->setStopOnLastFrame(true);
			}
			_sticker->initSize(data.size);
			_sticker->setCustomCachingTag(data.cacheTag);
		}
	}
}

DynamicImagePart::DynamicImagePart(
	not_null<Element*> parent,
	std::shared_ptr<Ui::DynamicImage> image,
	int size,
	QMargins margins,
	ClickHandlerPtr link,
	bool communityEffect)
: _parent(parent)
, _image(std::move(image))
, _link(std::move(link))
, _margins(margins)
, _size(size)
, _communityEffect(communityEffect) {
}

DynamicImagePart::~DynamicImagePart() = default;

void DynamicImagePart::repaintImage(uint64 generation) const {
	if (generation == _imageGeneration) {
		requestAnimationRepaint(_parent, _imageRepaint);
	}
}

void DynamicImagePart::draw(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth) const {
	const auto left = (outerWidth - _size) / 2;
	const auto top = _margins.top();
	const auto imageRect = QRect(left, top, _size, _size);
	recordAnimationRepaint(
		_parent,
		p,
		context,
		_imageRepaint,
		imageRect);
	if (!_subscribed) {
		_subscribed = 1;
		const auto generation = ++_imageGeneration;
		const auto weak = base::make_weak(this);
		_image->subscribeToUpdates([weak, generation] {
			const auto strong = weak.get();
			if (strong) {
				strong->repaintImage(generation);
			}
		});
		_parent->history()->owner().registerHeavyViewPart(_parent);
	}
	if (_communityEffect) {
		if (!_communityCache) {
			_communityCache = std::make_unique<Ui::CommunityUserpicEffect>();
		}
		Ui::PaintCommunityUserpicEffect(
			p,
			*_communityCache,
			left,
			top,
			_size,
			context.st->msgServiceBg()->c);
	}
	p.drawImage(QPoint(left, top), _image->image(_size));
}

TextState DynamicImagePart::textState(
		QPoint point,
		StateRequest request,
		int outerWidth) const {
	auto result = TextState(_parent);
	const auto left = (outerWidth - _size) / 2;
	if (_link && QRect(left, _margins.top(), _size, _size).contains(point)) {
		result.link = _link;
	}
	return result;
}

bool DynamicImagePart::hasHeavyPart() {
	return _subscribed;
}

void DynamicImagePart::unloadHeavyPart() {
	if (_subscribed) {
		_subscribed = 0;
		++_imageGeneration;
		_image->subscribeToUpdates(nullptr);
	}
	_communityCache = nullptr;
	resetAnimationRepaint(_imageRepaint);
}

QSize DynamicImagePart::countOptimalSize() {
	return {
		_margins.left() + _size + _margins.right(),
		_margins.top() + _size + _margins.bottom(),
	};
}

QSize DynamicImagePart::countCurrentSize(int newWidth) {
	if (_repaintWidth != newWidth) {
		_repaintWidth = newWidth;
		invalidateAnimationRepaint(_imageRepaint);
	}
	return { newWidth, minHeight() };
}

StickerWithBadgePart::StickerWithBadgePart(
	not_null<Element*> parent,
	Element *replacing,
	Fn<Data()> lookup,
	QMargins padding,
	QString badge,
	QImage customLeftIcon,
	std::optional<QColor> colorOverride)
: _customLeftIcon(std::move(customLeftIcon))
, _sticker(parent, replacing, std::move(lookup), padding)
, _badgeText(badge)
, _colorOverride(std::move(colorOverride)) {
}

void StickerWithBadgePart::draw(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth) const {
	_sticker.draw(p, owner, context, outerWidth);
	if (_sticker.resolved()) {
		paintBadge(p, context);
	}
}

TextState StickerWithBadgePart::textState(
		QPoint point,
		StateRequest request,
		int outerWidth) const {
	return _sticker.textState(point, request, outerWidth);
}

bool StickerWithBadgePart::hasHeavyPart() {
	return _sticker.hasHeavyPart();
}

void StickerWithBadgePart::unloadHeavyPart() {
	_sticker.unloadHeavyPart();
}

std::unique_ptr<StickerPlayer> StickerWithBadgePart::stickerTakePlayer(
		not_null<DocumentData*> data,
		const Lottie::ColorReplacements *replacements) {
	return _sticker.stickerTakePlayer(data, replacements);
}

QSize StickerWithBadgePart::countOptimalSize() {
	_sticker.initDimensions();
	return { _sticker.maxWidth(), _sticker.minHeight() };
}

QSize StickerWithBadgePart::countCurrentSize(int newWidth) {
	return _sticker.countCurrentSize(newWidth);
}

void StickerWithBadgePart::paintBadge(
		Painter &p,
		const PaintContext &context) const {
	validateBadge(context);

	const auto badge = _badge.size() / _badge.devicePixelRatio();
	const auto left = (width() - badge.width()) / 2;
	const auto top = st::chatGiveawayBadgeTop;
	const auto rect = QRect(left, top, badge.width(), badge.height());
	const auto paintContent = [&](QPainter &q) {
		q.drawImage(rect.topLeft(), _badge);
	};

	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		if (_colorOverride) {
			p.setBrush(*_colorOverride);
		} else {
			p.setBrush(context.messageStyle()->msgFileBg);
		}
		const auto half = st::chatGiveawayBadgeStroke / 2.;
		const auto inner = QRectF(rect) - Margins(half);
		const auto radius = inner.height() / 2.;
		p.drawRoundedRect(inner, radius, radius);
		if (_colorOverride && context.selected()) {
			p.setBrush(context.st->msgStickerOverlay());
			p.drawRoundedRect(inner, radius, radius);
		}
	}

	if (!_sticker.parent()->usesBubblePattern(context)) {
		paintContent(p);
	} else {
		Ui::PaintPatternBubblePart(
			p,
			context.viewport,
			context.bubblesPattern->pixmap,
			rect,
			paintContent,
			_badgeCache);
	}
}

void StickerWithBadgePart::validateBadge(
		const PaintContext &context) const {
	const auto stm = context.messageStyle();
	const auto &badgeFg = st::premiumButtonFg->c;
	const auto &badgeBorder = stm->msgBg->c;
	if (!_badge.isNull()
		&& _badgeFg == badgeFg
		&& _badgeBorder == badgeBorder) {
		return;
	}
	const auto &font = st::chatGiveawayBadgeFont;
	_badgeFg = badgeFg;
	_badgeBorder = badgeBorder;
	const auto iconWidth = _customLeftIcon.isNull()
		? 0
		: (_customLeftIcon.width() / style::DevicePixelRatio());
	const auto width = font->width(_badgeText) + iconWidth;
	const auto inner = QRect(0, 0, width, font->height);
	const auto rect = inner + st::chatGiveawayBadgePadding;
	const auto size = rect.size();
	const auto ratio = style::DevicePixelRatio();
	_badge = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	_badge.setDevicePixelRatio(ratio);
	_badge.fill(Qt::transparent);

	auto p = QPainter(&_badge);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(QPen(_badgeBorder, st::chatGiveawayBadgeStroke * 1.));
	p.setBrush(Qt::NoBrush);
	const auto half = st::chatGiveawayBadgeStroke / 2.;
	const auto left = _customLeftIcon.isNull()
		? st::chatGiveawayBadgePadding.left()
		: (st::chatGiveawayBadgePadding.left() - half * 2);
	const auto smaller = QRectF(rect.translated(-rect.topLeft()))
		- Margins(half);
	const auto radius = smaller.height() / 2.;
	p.drawRoundedRect(smaller, radius, radius);
	p.setPen(_badgeFg);
	p.setFont(font);
	p.drawText(
		left + iconWidth,
		st::chatGiveawayBadgePadding.top() + font->ascent,
		_badgeText);
	if (!_customLeftIcon.isNull()) {
		const auto iconHeight = _customLeftIcon.height()
			/ style::DevicePixelRatio();
		p.drawImage(
			left,
			half + (inner.height() - iconHeight) / 2,
			context.selected()
				? Images::Colored(base::duplicate(_customLeftIcon), _badgeFg)
				: _customLeftIcon);
	}
}

PeerBubbleListPart::PeerBubbleListPart(
	not_null<Element*> parent,
	const std::vector<not_null<PeerData*>> &list)
: _parent(parent) {
	for (const auto &peer : list) {
		_peers.push_back({
			.name = Ui::Text::String(
				st::semiboldTextStyle,
				peer->name(),
				kDefaultTextOptions,
				st::msgMinWidth),
			.thumbnail = Ui::MakeUserpicThumbnail(peer),
			.link = peer->openLink(),
			.colorIndex = peer->colorIndex(),
		});
	}
}

PeerBubbleListPart::~PeerBubbleListPart() = default;

Fn<void()> PeerBubbleListPart::peerRepaintCallback(
		int index,
		uint64 generation,
		RepaintSource source) const {
	const auto weak = base::make_weak(this);
	return [weak, index, generation, source] {
		const auto strong = weak.get();
		if (strong) {
			strong->repaintPeer(index, generation, source);
		}
	};
}

void PeerBubbleListPart::repaintPeer(
		int index,
		uint64 generation,
		RepaintSource source) const {
	if (generation != _peersGeneration
		|| index < 0
		|| index >= int(_peers.size())) {
		return;
	}
	const auto &peer = _peers[index];
	auto &repaint = (source == RepaintSource::Content)
		? peer.contentRepaint
		: peer.rippleRepaint;
	requestAnimationRepaint(_parent, repaint);
}

void PeerBubbleListPart::recordPeerRepaint(
		const Painter &p,
		const PaintContext &context,
		int index,
		RepaintSource source,
		QRect rect) const {
	Assert(index >= 0 && index < int(_peers.size()));
	const auto &peer = _peers[index];
	auto &repaint = (source == RepaintSource::Content)
		? peer.contentRepaint
		: peer.rippleRepaint;
	recordAnimationRepaint(_parent, p, context, repaint, rect);
}

void PeerBubbleListPart::invalidatePeerRepaints(Peer &peer) const {
	invalidateAnimationRepaint(peer.contentRepaint);
	invalidateAnimationRepaint(peer.rippleRepaint);
}

void PeerBubbleListPart::resetPeerRepaints(Peer &peer) const {
	resetAnimationRepaint(peer.contentRepaint);
	resetAnimationRepaint(peer.rippleRepaint);
}

void PeerBubbleListPart::draw(
		Painter &p,
		not_null<const MediaGeneric*> owner,
		const PaintContext &context,
		int outerWidth) const {
	if (_peers.empty()) {
		return;
	}

	const auto size = _peers[0].geometry.height();
	const auto st = context.st;
	const auto stm = context.messageStyle();
	const auto selected = context.selected();
	const auto padding = st::chatGiveawayPeerPadding;
	const auto count = int(_peers.size());
	for (auto i = 0; i != count; ++i) {
		const auto &geometry = _peers[i].geometry;
		recordPeerRepaint(
			p,
			context,
			i,
			RepaintSource::Content,
			QRect(geometry.topLeft(), QSize(size, size)));
		recordPeerRepaint(
			p,
			context,
			i,
			RepaintSource::Ripple,
			style::rtlrect(geometry, width()));
	}
	if (!_subscribed) {
		_subscribed = 1;
		const auto generation = ++_peersGeneration;
		for (auto i = 0; i != count; ++i) {
			_peers[i].thumbnail->subscribeToUpdates(peerRepaintCallback(
				i,
				generation,
				RepaintSource::Content));
		}
		_parent->history()->owner().registerHeavyViewPart(_parent);
	}
	for (auto i = 0; i != count; ++i) {
		const auto &peer = _peers[i];
		const auto &thumbnail = peer.thumbnail;
		const auto &geometry = peer.geometry;

		const auto colorIndex = peer.colorIndex;
		const auto cache = context.outbg
			? stm->replyCache[st->colorPatternIndex(colorIndex)].get()
			: st->coloredReplyCache(selected, colorIndex).get();
		if (peer.corners[0].isNull() || peer.bg != cache->bg) {
			peer.bg = cache->bg;
			peer.corners = Images::CornersMask(size / 2);
			for (auto &image : peer.corners) {
				style::colorizeImage(image, cache->bg, &image);
			}
		}
		p.setPen(cache->icon);
		Ui::DrawRoundedRect(p, geometry, peer.bg, peer.corners);
		if (peer.ripple) {
			peer.ripple->paint(
				p,
				geometry.x(),
				geometry.y(),
				width(),
				&cache->bg);
			if (peer.ripple->empty()) {
				peer.ripple = nullptr;
			}
		}

		p.drawImage(geometry.topLeft(), thumbnail->image(size));
		const auto left = size + padding.left();
		const auto top = padding.top();
		const auto available = geometry.width() - left - padding.right();
		peer.name.draw(p, {
			.position = { geometry.left() + left, geometry.top() + top },
			.outerWidth = width(),
			.availableWidth = available,
			.align = style::al_left,
			.palette = &stm->textPalette,
			.now = context.now,
			.elisionLines = 1,
			.elisionBreakEverywhere = true,
		});
	}
}

int PeerBubbleListPart::layout(int x, int y, int available) {
	auto previous = std::vector<QRect>();
	previous.reserve(_peers.size());
	for (const auto &peer : _peers) {
		previous.push_back(peer.geometry);
	}
	const auto size = st::chatGiveawayPeerSize;
	const auto skip = st::chatGiveawayPeerSkip;
	const auto padding = st::chatGiveawayPeerPadding;
	auto left = available;
	const auto shiftRow = [&](int i, int top, int shift) {
		for (auto j = i; j != 0; --j) {
			auto &geometry = _peers[j - 1].geometry;
			if (geometry.top() != top) {
				break;
			}
			geometry.moveLeft(geometry.x() + shift);
		}
	};
	const auto count = int(_peers.size());
	for (auto i = 0; i != count; ++i) {
		const auto desired = size
			+ padding.left()
			+ _peers[i].name.maxWidth()
			+ padding.right();
		const auto width = std::min(desired, available);
		if (left < width) {
			shiftRow(i, y, (left + skip) / 2);
			left = available;
			y += size + skip;
		}
		_peers[i].geometry = { x + available - left, y, width, size };
		left -= width + skip;
	}
	shiftRow(count, y, (left + skip) / 2);
	for (auto i = 0; i != count; ++i) {
		auto &peer = _peers[i];
		if (peer.geometry == previous[i]) {
			continue;
		}
		invalidatePeerRepaints(peer);
		if (peer.geometry.size() != previous[i].size()) {
			peer.ripple = nullptr;
		}
	}
	return y + size + skip;
}

TextState PeerBubbleListPart::textState(
		QPoint point,
		StateRequest request,
		int outerWidth) const {
	auto result = TextState(_parent);
	for (const auto &peer : _peers) {
		if (peer.geometry.contains(point)) {
			result.link = peer.link;
			_lastPoint = point;
			break;
		}
	}
	return result;
}

void PeerBubbleListPart::clickHandlerPressedChanged(
		const ClickHandlerPtr &p,
		bool pressed) {
	for (auto i = 0; i != int(_peers.size()); ++i) {
		auto &peer = _peers[i];
		if (peer.link != p) {
			continue;
		}
		if (pressed) {
			if (!peer.ripple) {
				peer.ripple = std::make_unique<Ui::RippleAnimation>(
					st::defaultRippleAnimation,
					Ui::RippleAnimation::RoundRectMask(
						peer.geometry.size(),
						peer.geometry.height() / 2),
					peerRepaintCallback(
						i,
						_peersGeneration,
						RepaintSource::Ripple));
			}
			peer.ripple->add(_lastPoint - peer.geometry.topLeft());
		} else if (peer.ripple) {
			peer.ripple->lastStop();
		}
		break;
	}
}

bool PeerBubbleListPart::hasHeavyPart() {
	return _subscribed;
}

void PeerBubbleListPart::unloadHeavyPart() {
	++_peersGeneration;
	if (_subscribed) {
		_subscribed = 0;
		for (const auto &peer : _peers) {
			peer.thumbnail->subscribeToUpdates(nullptr);
		}
	}
	for (auto &peer : _peers) {
		peer.ripple = nullptr;
		resetPeerRepaints(peer);
	}
}

QSize PeerBubbleListPart::countOptimalSize() {
	if (_peers.empty()) {
		return {};
	}
	const auto size = st::chatGiveawayPeerSize;
	const auto skip = st::chatGiveawayPeerSkip;
	const auto padding = st::chatGiveawayPeerPadding;
	auto left = st::msgPadding.left();
	for (const auto &peer : _peers) {
		const auto desired = size
			+ padding.left()
			+ peer.name.maxWidth()
			+ padding.right();
		left += desired + skip;
	}
	return { left - skip + st::msgPadding.right(), size };
}

QSize PeerBubbleListPart::countCurrentSize(int newWidth) {
	if (_peers.empty()) {
		return {};
	}
	const auto padding = st::msgPadding;
	const auto available = newWidth - padding.left() - padding.right();
	const auto channelsBottom = layout(
		padding.left(),
		0,
		available);
	return { newWidth, channelsBottom };
}

} // namespace HistoryView
