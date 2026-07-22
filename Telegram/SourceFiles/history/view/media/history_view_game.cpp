/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_game.h"

#include "lang/lang_keys.h"
#include "history/history_item_components.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/media/history_view_media_common.h"
#include "ui/chat/chat_style.h"
#include "ui/effects/ripple_animation.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/text/text_utilities.h"
#include "ui/cached_round_corners.h"
#include "ui/damage_debug.h"
#include "ui/item_text_options.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "core/ui_integration.h"
#include "data/data_session.h"
#include "data/data_game.h"
#include "data/data_media_types.h"
#include "styles/style_chat.h"

namespace HistoryView {

Game::Game(
	not_null<Element*> parent,
	not_null<GameData*> data,
	const TextWithEntities &consumed)
: Media(parent)
, _st(st::historyPagePreview)
, _data(data)
, _title(st::msgMinWidth - _st.padding.left() - _st.padding.right())
, _description(st::msgMinWidth - _st.padding.left() - _st.padding.right()) {
	if (!consumed.text.isEmpty()) {
		setDescription(consumed);
	}
	history()->owner().registerGameView(_data, _parent);
}

QSize Game::countOptimalSize() {
	invalidateDescriptionRepaint();
	invalidateRippleRepaint();
	auto lineHeight = UnitedLineHeight();

	const auto item = _parent->data();
	if (!_openl && item->isRegular()) {
		const auto row = 0;
		const auto column = 0;
		_openl = std::make_shared<ReplyMarkupClickHandler>(
			&item->history()->owner(),
			row,
			column,
			item->fullId());
	}

	auto title = TextUtilities::SingleLine(_data->title);

	// init attach
	if (!_attach) {
		_attach = CreateAttach(
			_parent,
			_data->document,
			_data->document ? nullptr : _data->photo);
	}

	// init strings
	if (_description.isEmpty() && !_data->description.isEmpty()) {
		auto text = _data->description;
		if (!text.isEmpty()) {
			auto marked = TextWithEntities { text };
			auto parseFlags = TextParseLinks | TextParseMultiline;
			TextUtilities::ParseEntities(marked, parseFlags);
			_description.setMarkedText(
				st::webPageDescriptionStyle,
				marked,
				Ui::WebpageTextDescriptionOptions());
			if (!_attach) {
				_description.updateSkipBlock(
					_parent->skipBlockWidth(),
					_parent->skipBlockHeight());
			}
		}
	}
	if (_title.isEmpty() && !title.isEmpty()) {
		_title.setText(
			st::webPageTitleStyle,
			title,
			Ui::WebpageTextTitleOptions());
	}

	// init dimensions
	auto skipBlockWidth = _parent->skipBlockWidth();
	auto maxWidth = skipBlockWidth;
	auto minHeight = 0;

	auto titleMinHeight = _title.isEmpty() ? 0 : lineHeight;
	// enable any count of lines in game description / message
	auto descMaxLines = 4096;
	auto descriptionMinHeight = _description.isEmpty() ? 0 : qMin(_description.minHeight(), descMaxLines * lineHeight);

	if (!_title.isEmpty()) {
		accumulate_max(maxWidth, _title.maxWidth());
		minHeight += titleMinHeight;
	}
	if (!_description.isEmpty()) {
		accumulate_max(maxWidth, _description.maxWidth());
		minHeight += descriptionMinHeight;
	}
	if (_attach) {
		auto attachAtTop = !_titleLines && !_descriptionLines;
		if (!attachAtTop) minHeight += st::mediaInBubbleSkip;

		_attach->initDimensions();
		QMargins bubble(_attach->bubbleMargins());
		const auto maxMediaWidth = _attach->maxWidth()
			- bubble.left()
			- bubble.right();
		accumulate_max(maxWidth, maxMediaWidth);
		minHeight += _attach->minHeight() - bubble.top() - bubble.bottom();
	}
	auto padding = inBubblePadding() + innerMargin();
	maxWidth += padding.left() + padding.right();
	minHeight += padding.top() + padding.bottom();

	if (!_gameTagWidth) {
		_gameTagWidth = st::msgDateFont->width(tr::lng_game_tag(tr::now).toUpper());
	}
	return { maxWidth, minHeight };
}

void Game::refreshParentId(not_null<HistoryItem*> realParent) {
	if (_openl) {
		_openl->setMessageId(realParent->fullId());
	}
	if (_attach) {
		_attach->refreshParentId(realParent);
	}
}

bool Game::playbackUpdated(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) const {
	return _attach && _attach->playbackUpdated(item, document);
}

void Game::transferUpdated(
		not_null<const HistoryItem*> item,
		not_null<const PhotoData*> photo) const {
	if (_attach) {
		_attach->transferUpdated(item, photo);
	}
}

void Game::transferUpdated(
		not_null<const HistoryItem*> item,
		not_null<const DocumentData*> document) const {
	if (_attach) {
		_attach->transferUpdated(item, document);
	}
}

QSize Game::countCurrentSize(int newWidth) {
	invalidateDescriptionRepaint();
	invalidateRippleRepaint();
	accumulate_min(newWidth, maxWidth());
	const auto padding = inBubblePadding() + innerMargin();
	auto innerWidth = newWidth - padding.left() - padding.right();

	// enable any count of lines in game description / message
	auto linesMax = 4096;
	auto lineHeight = UnitedLineHeight();
	auto newHeight = 0;
	if (_title.isEmpty()) {
		_titleLines = 0;
	} else {
		if (_title.countHeight(innerWidth) < 2 * st::webPageTitleFont->height) {
			_titleLines = 1;
		} else {
			_titleLines = 2;
		}
		newHeight += _titleLines * lineHeight;
	}

	if (_description.isEmpty()) {
		_descriptionLines = 0;
	} else {
		auto descriptionHeight = _description.countHeight(innerWidth);
		if (descriptionHeight < (linesMax - _titleLines) * st::webPageDescriptionFont->height) {
			_descriptionLines = (descriptionHeight / st::webPageDescriptionFont->height);
		} else {
			_descriptionLines = (linesMax - _titleLines);
		}
		newHeight += _descriptionLines * lineHeight;
	}

	if (_attach) {
		auto attachAtTop = !_titleLines && !_descriptionLines;
		if (!attachAtTop) newHeight += st::mediaInBubbleSkip;

		QMargins bubble(_attach->bubbleMargins());

		_attach->resizeGetHeight(innerWidth + bubble.left() + bubble.right());
		newHeight += _attach->height() - bubble.top() - bubble.bottom();
	}
	newHeight += padding.top() + padding.bottom();

	return { newWidth, newHeight };
}

TextSelection Game::toDescriptionSelection(
		TextSelection selection) const {
	return UnshiftItemSelection(selection, _title);
}

TextSelection Game::fromDescriptionSelection(
		TextSelection selection) const {
	return ShiftItemSelection(selection, _title);
}

void Game::draw(Painter &p, const PaintContext &context) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		recordDescriptionRepaintRect(
			p,
			context,
			QRect(),
			0,
			0,
			Ui::Text::CustomEmojiRepaintBounds());
		recordRippleRepaintRect(p, context, QRect());
		return;
	}

	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto stm = context.messageStyle();

	const auto bubble = _attach ? _attach->bubbleMargins() : QMargins();
	const auto full = QRect(0, 0, width(), height());
	auto outer = full.marginsRemoved(inBubblePadding());
	auto inner = outer.marginsRemoved(innerMargin());
	if (context.hasElementPainter(p)
		&& _ripple
		&& _rippleSize != outer.size()) {
		_ripple = nullptr;
		_rippleSize = QSize();
		++_rippleRepaint.generation;
	}
	recordRippleRepaintRect(p, context, outer);
	auto tshift = inner.top();
	auto paintw = inner.width();
	const auto selected = context.selected();
	const auto colorIndex = parent()->contentColorIndex();
	const auto &colorCollectible = parent()->contentColorCollectible();
	const auto colorPattern = colorCollectible
		? st->collectiblePatternIndex(colorCollectible)
		: st->colorPatternIndex(colorIndex);
	const auto useColorCollectible = colorCollectible && !context.outbg;
	const auto useColorIndex = !context.outbg;
	const auto cache = useColorCollectible
		? st->collectibleReplyCache(selected, colorCollectible).get()
		: useColorIndex
		? st->coloredReplyCache(selected, colorIndex).get()
		: stm->replyCache[colorPattern].get();
	Ui::Text::ValidateQuotePaintCache(*cache, _st);
	Ui::Text::FillQuotePaint(p, outer, *cache, _st);

	if (_ripple) {
		p.save();
		p.translate(outer.topLeft());
		_ripple->paint(p, 0, 0, _rippleSize.width(), &cache->bg);
		p.restore();
		if (_ripple->empty()) {
			_ripple = nullptr;
			_rippleSize = QSize();
			++_rippleRepaint.generation;
		}
	}

	auto lineHeight = UnitedLineHeight();
	if (_titleLines) {
		p.setPen(cache->icon);
		p.setTextPalette(useColorCollectible
			? st->collectibleTextPalette(selected, colorCollectible)
			: useColorIndex
			? st->coloredTextPalette(selected, colorIndex)
			: stm->semiboldPalette);

		auto endskip = 0;
		if (_title.hasSkipBlock()) {
			endskip = _parent->skipBlockWidth();
		}
		_title.drawLeftElided(
			p,
			inner.left(),
			tshift,
			paintw,
			width(),
			_titleLines,
			style::al_left,
			0,
			-1,
			endskip,
			false,
			context.selection);
		tshift += _titleLines * lineHeight;

		p.setTextPalette(stm->textPalette);
	}
	if (_descriptionLines) {
		p.setPen(stm->historyTextFg);
		auto endskip = 0;
		if (_description.hasSkipBlock()) {
			endskip = _parent->skipBlockWidth();
		}
		_parent->prepareCustomEmojiPaint(p, context, _description);
		auto customEmojiRepaintBounds
			= Ui::Text::CustomEmojiRepaintBounds();
		_description.draw(p, {
			.position = { inner.left(), tshift },
			.outerWidth = width(),
			.availableWidth = paintw,
			.spoiler = Ui::Text::DefaultSpoilerCache(),
			.now = context.now,
			.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
			.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
			.selection = toDescriptionSelection(context.selection),
			.elisionHeight = _descriptionLines * lineHeight,
			.elisionRemoveFromEnd = endskip,
			.useFullWidth = true,
			.customEmojiRepaintBounds = &customEmojiRepaintBounds,
		});
		recordDescriptionRepaintRect(
			p,
			context,
			QRect(
				inner.left(),
				tshift,
				paintw,
				_descriptionLines * lineHeight),
			_descriptionLines,
			endskip,
			customEmojiRepaintBounds);
		tshift += _descriptionLines * lineHeight;
	} else {
		recordDescriptionRepaintRect(
			p,
			context,
			QRect(),
			0,
			0,
			Ui::Text::CustomEmojiRepaintBounds());
	}
	if (_attach) {
		auto attachAtTop = !_titleLines && !_descriptionLines;
		if (!attachAtTop) tshift += st::mediaInBubbleSkip;

		auto attachLeft = inner.left() - bubble.left();
		auto attachTop = tshift - bubble.top();
		if (rtl()) attachLeft = width() - attachLeft - _attach->width();

		p.translate(attachLeft, attachTop);
		_attach->draw(p, context.translated(
			-attachLeft,
			-attachTop
		).withSelection(context.selected()
			? FullSelection
			: TextSelection()));
		auto pixwidth = _attach->width();
		auto pixheight = _attach->height();

		auto gameW = _gameTagWidth + 2 * st::msgDateImgPadding.x();
		auto gameH = st::msgDateFont->height + 2 * st::msgDateImgPadding.y();
		auto gameX = pixwidth - st::msgDateImgDelta - gameW;
		auto gameY = pixheight - st::msgDateImgDelta - gameH;

		Ui::FillRoundRect(p, style::rtlrect(gameX, gameY, gameW, gameH, pixwidth), sti->msgDateImgBg, sti->msgDateImgBgCorners);

		p.setFont(st::msgDateFont);
		p.setPen(st->msgDateImgFg());
		p.drawTextLeft(gameX + st::msgDateImgPadding.x(), gameY + st::msgDateImgPadding.y(), pixwidth, tr::lng_game_tag(tr::now).toUpper());

		p.translate(-attachLeft, -attachTop);
	}
}

TextState Game::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);

	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return result;
	}

	const auto bubble = _attach ? _attach->bubbleMargins() : QMargins();
	const auto full = QRect(0, 0, width(), height());
	auto outer = full.marginsRemoved(inBubblePadding());
	auto inner = outer.marginsRemoved(innerMargin());
	auto tshift = inner.top();
	auto paintw = inner.width();

	auto symbolAdd = 0;
	auto lineHeight = UnitedLineHeight();
	if (_titleLines) {
		if (point.y() >= tshift && point.y() < tshift + _titleLines * lineHeight) {
			Ui::Text::StateRequestElided titleRequest = request.forText();
			titleRequest.lines = _titleLines;
			result = TextState(_parent, _title.getStateElidedLeft(
				point - QPoint(inner.left(), tshift),
				paintw,
				width(),
				titleRequest));
		} else if (point.y() >= tshift + _titleLines * lineHeight) {
			symbolAdd += _title.length();
		}
		tshift += _titleLines * lineHeight;
	}
	if (_descriptionLines) {
		if (point.y() >= tshift && point.y() < tshift + _descriptionLines * lineHeight) {
			Ui::Text::StateRequestElided descriptionRequest = request.forText();
			descriptionRequest.lines = _descriptionLines;
			result = TextState(_parent, _description.getStateElidedLeft(
				point - QPoint(inner.left(), tshift),
				paintw,
				width(),
				descriptionRequest));
		} else if (point.y() >= tshift + _descriptionLines * lineHeight) {
			symbolAdd += _description.length();
		}
		tshift += _descriptionLines * lineHeight;
	}
	if (_attach) {
		auto attachAtTop = !_titleLines && !_descriptionLines;
		if (!attachAtTop) tshift += st::mediaInBubbleSkip;

		auto attachLeft = inner.left() - bubble.left();
		auto attachTop = tshift - bubble.top();
		if (rtl()) attachLeft = width() - attachLeft - _attach->width();

		if (QRect(attachLeft, tshift, _attach->width(), inner.top() + inner.height() - tshift).contains(point)) {
			if (_attach->isReadyForOpen()) {
				if (_parent->data()->isHistoryEntry()) {
					result.link = _openl;
				}
			} else {
				result = _attach->textState(point - QPoint(attachLeft, attachTop), request);
			}
		}
	}
	if (_parent->data()->isHistoryEntry()) {
		if (!result.link && outer.contains(point)) {
			result.link = _openl;
		}
	}
	_lastPoint = point - outer.topLeft();

	result.symbol += symbolAdd;
	return result;
}

TextSelection Game::adjustSelection(TextSelection selection, TextSelectType type) const {
	if (!_descriptionLines || selection.to <= _title.length()) {
		return _title.adjustSelection(selection, type);
	}
	auto descriptionSelection = _description.adjustSelection(toDescriptionSelection(selection), type);
	if (selection.from >= _title.length()) {
		return fromDescriptionSelection(descriptionSelection);
	}
	auto titleSelection = _title.adjustSelection(selection, type);
	return { titleSelection.from, fromDescriptionSelection(descriptionSelection).to };
}

void Game::clickHandlerActiveChanged(const ClickHandlerPtr &p, bool active) {
	if (_attach) {
		_attach->clickHandlerActiveChanged(p, active);
	}
}

void Game::clickHandlerPressedChanged(const ClickHandlerPtr &p, bool pressed) {
	if (p == _openl) {
		if (pressed) {
			if (!_ripple) {
				const auto full = QRect(0, 0, width(), height());
				const auto outer = full.marginsRemoved(inBubblePadding());
				const auto weak = base::make_weak(this);
				const auto generation = resetRippleRepaint();
				_rippleSize = outer.size();
				_ripple = std::make_unique<Ui::RippleAnimation>(
					st::defaultRippleAnimation,
					Ui::RippleAnimation::RoundRectMask(
						_rippleSize,
						_st.radius),
					[weak, generation] {
						if (const auto strong = weak.get()) {
							strong->repaintRipple(generation);
						}
					});
			}
			_ripple->add(_lastPoint);
		} else if (_ripple) {
			_ripple->lastStop();
		}
	}
	if (_attach) {
		_attach->clickHandlerPressedChanged(p, pressed);
	}
}

bool Game::toggleSelectionByHandlerClick(const ClickHandlerPtr &p) const {
	return _attach && _attach->toggleSelectionByHandlerClick(p);
}

bool Game::allowTextSelectionByHandler(const ClickHandlerPtr &p) const {
	return (p == _openl);
}

bool Game::dragItemByHandler(const ClickHandlerPtr &p) const {
	return _attach && _attach->dragItemByHandler(p);
}

TextForMimeData Game::selectedText(TextSelection selection) const {
	auto titleResult = _title.toTextForMimeData(selection);
	auto descriptionResult = _description.toTextForMimeData(
		toDescriptionSelection(selection));
	if (titleResult.empty()) {
		return descriptionResult;
	} else if (descriptionResult.empty()) {
		return titleResult;
	}
	return titleResult.append('\n').append(std::move(descriptionResult));
}

void Game::playAnimation(bool autoplay) {
	if (_attach) {
		if (autoplay) {
			_attach->autoplayAnimation();
		} else {
			_attach->playAnimation();
		}
	}
}

QMargins Game::inBubblePadding() const {
	return {
		st::msgPadding.left(),
		isBubbleTop() ? st::msgPadding.left() : st::mediaInBubbleSkip,
		st::msgPadding.right(),
		(isBubbleBottom()
			? (st::msgPadding.left() + bottomInfoPadding())
			: st::mediaInBubbleSkip),
	};
}

QMargins Game::innerMargin() const {
	return _st.padding;
}

int Game::bottomInfoPadding() const {
	if (!isBubbleBottom()) {
		return 0;
	}

	auto result = st::msgDateFont->height;

	// we use padding greater than st::msgPadding.bottom() in the
	// bottom of the bubble so that the left line looks pretty.
	// but if we have bottom skip because of the info display
	// we don't need that additional padding so we replace it
	// back with st::msgPadding.bottom() instead of left().
	result += st::msgPadding.bottom() - st::msgPadding.left();
	return result;
}

void Game::parentTextUpdated() {
	if (const auto media = _parent->data()->media()) {
		setDescription(media->consumedMessageText());
		history()->owner().requestViewResize(_parent);
	}
}

void Game::setDescription(const TextWithEntities &description) {
	invalidateDescriptionRepaint();
	const auto generation = ++_descriptionRepaint.generation;
	if (description.text.isEmpty()) {
		_description = Ui::Text::String(st::msgMinWidth
			- _st.padding.left()
			- _st.padding.right());
		return;
	}
	_description.setMarkedText(
		st::webPageDescriptionStyle,
		description,
		Ui::ItemTextOptions(_parent->data()),
		Core::TextContext({
			.session = &history()->session(),
			.repaint = descriptionRepaintCallback(generation),
		}));
}

Fn<void()> Game::descriptionRepaintCallback(uint64 generation) {
	const auto weak = base::make_weak(this);
	return [weak, generation] {
		if (const auto strong = weak.get()) {
			strong->repaintDescription(generation);
		}
	};
}

void Game::repaintDescription(uint64 generation) const {
	if (_descriptionRepaint.generation != generation
		|| _descriptionRepaint.pending) {
		return;
	} else if (_descriptionRepaint.known
		&& _descriptionRepaint.current.isEmpty()) {
		return;
	}
	_descriptionRepaint.pending = true;
	if (!_descriptionRepaint.known) {
		Ui::LogUnknownGeometryRepaint("game description");
		_parent->customEmojiRepaint();
	} else {
		repaintDescriptionRegion(_descriptionRepaint.current);
	}
}

void Game::recordDescriptionRepaintRect(
		const Painter &p,
		const PaintContext &context,
		QRect rect,
		int visibleLines,
		int removeFromEnd,
		const Ui::Text::CustomEmojiRepaintBounds
			&customEmojiRepaintBounds) const {
	if (!context.hasElementPainter(p)) {
		if (!_descriptionRepaint.known) {
			if (_descriptionRepaint.pending) {
				_parent->clearCustomEmojiRepaint();
			}
			_descriptionRepaint.pending = false;
		}
		return;
	}
	if (_descriptionRepaint.pending && !_descriptionRepaint.known) {
		_parent->clearCustomEmojiRepaint();
	}
	_descriptionRepaint.pending = false;
	auto region = QRegion();
	auto geometryKnown = true;
	if (!customEmojiRepaintBounds.rect.isEmpty()) {
		const auto mapped = context.mapToElement(
			p,
			customEmojiRepaintBounds.rect);
		if (!mapped) {
			geometryKnown = false;
		} else if (!mapped->isEmpty()) {
			region += *mapped;
		}
	}
	const auto needsTextFallback
		= !customEmojiRepaintBounds.repaintBoundsKnown
		|| _description.hasSpoilers();
	if (geometryKnown && needsTextFallback) {
		if (rect.isEmpty() || visibleLines <= 0) {
			geometryKnown = customEmojiRepaintBounds.repaintBoundsKnown;
		}
	}
	if (geometryKnown
		&& needsTextFallback
		&& !rect.isEmpty()
		&& visibleLines > 0) {
		const auto lines = _description.countLinesGeometry(rect.width());
		const auto count = std::min(visibleLines, int(lines.size()));
		const auto conservativeLast = removeFromEnd > 0
			|| int(lines.size()) > visibleLines;
		auto lineTop = 0;
		if (!count) {
			geometryKnown = false;
		}
		for (auto i = 0; i != count; ++i) {
			const auto lineBottom = std::clamp(
				lines[i].bottom,
				lineTop,
				rect.height());
			auto lineLeft = std::clamp(
				lines[i].left,
				0,
				rect.width());
			auto lineWidth = std::clamp(
				lines[i].width,
				0,
				rect.width());
			if ((i + 1 == count) && conservativeLast) {
				lineLeft = 0;
				lineWidth = rect.width();
			} else if (lines[i].rtl) {
				lineLeft = rect.width() - lineWidth;
			} else {
				lineWidth = std::min(
					lineWidth,
					rect.width() - lineLeft);
			}
			if (lineWidth > 0 && lineBottom > lineTop) {
				const auto lineRect = QRectF(
					rect.x() + lineLeft,
					rect.y() + lineTop,
					lineWidth,
					lineBottom - lineTop);
				const auto mapped = context.mapToElement(p, lineRect);
				if (!mapped) {
					geometryKnown = false;
					break;
				} else if (!mapped->isEmpty()) {
					region += *mapped;
				}
			}
			lineTop = lineBottom;
		}
	}
	const auto stale = base::take(_descriptionRepaint.stale);
	const auto previous = stale.united(
		base::take(_descriptionRepaint.current));
	_descriptionRepaint.current = geometryKnown
		? std::move(region)
		: QRegion();
	_descriptionRepaint.known = geometryKnown;
	if (!geometryKnown) {
		_descriptionRepaint.stale = previous;
		if (stale.isEmpty() && !previous.isEmpty()) {
			_descriptionRepaint.pending = true;
			repaintDescriptionRegion(previous);
		}
		return;
	}
	if (previous.isEmpty()
		|| (stale.isEmpty()
			&& previous == _descriptionRepaint.current)) {
		return;
	}
	_descriptionRepaint.pending = true;
	repaintDescriptionRegion(
		previous.united(_descriptionRepaint.current));
}

void Game::invalidateDescriptionRepaint() const {
	if (_descriptionRepaint.pending && !_descriptionRepaint.known) {
		_parent->clearCustomEmojiRepaint();
	}
	_descriptionRepaint.stale = _descriptionRepaint.stale.united(
		base::take(_descriptionRepaint.current));
	_descriptionRepaint.pending = false;
	_descriptionRepaint.known = false;
}

void Game::repaintDescriptionRegion(const QRegion &region) const {
	_parent->repaint(region);
}

void Game::repaintRipple(uint64 generation) const {
	if (_rippleRepaint.generation != generation
		|| _rippleRepaint.pending
		|| (_rippleRepaint.known && _rippleRepaint.current.isEmpty())) {
		return;
	}
	_rippleRepaint.pending = true;
	if (!_rippleRepaint.known) {
		Ui::LogUnknownGeometryRepaint("game ripple");
		this->repaint();
	} else {
		repaintRippleRegion(_rippleRepaint.current);
	}
}

void Game::recordRippleRepaintRect(
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
	const auto stale = base::take(_rippleRepaint.stale);
	const auto previous = stale.united(
		base::take(_rippleRepaint.current));
	_rippleRepaint.pending = false;
	_rippleRepaint.current = known ? std::move(current) : QRegion();
	_rippleRepaint.known = known;
	if (!known) {
		_rippleRepaint.stale = previous;
		if (!previous.isEmpty()) {
			_rippleRepaint.pending = true;
			Ui::LogUnknownGeometryRepaint("game ripple");
			this->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == _rippleRepaint.current)) {
		return;
	}
	_rippleRepaint.pending = true;
	repaintRippleRegion(previous.united(_rippleRepaint.current));
}

void Game::invalidateRippleRepaint() const {
	_rippleRepaint.stale = _rippleRepaint.stale.united(
		base::take(_rippleRepaint.current));
	_rippleRepaint.pending = false;
	_rippleRepaint.known = false;
}

void Game::repaintRippleRegion(const QRegion &region) const {
	_parent->repaint(region);
}

uint64 Game::resetRippleRepaint() const {
	_rippleRepaint.pending = false;
	return ++_rippleRepaint.generation;
}

bool Game::hasHeavyPart() const {
	return _attach ? _attach->hasHeavyPart() : false;
}

void Game::unloadHeavyPart() {
	if (_attach) {
		_attach->unloadHeavyPart();
	}
	_description.unloadPersistentAnimation();
}

Game::~Game() {
	invalidateDescriptionRepaint();
	++_descriptionRepaint.generation;
	invalidateRippleRepaint();
	++_rippleRepaint.generation;
	history()->owner().unregisterGameView(_data, _parent);
}

} // namespace HistoryView
