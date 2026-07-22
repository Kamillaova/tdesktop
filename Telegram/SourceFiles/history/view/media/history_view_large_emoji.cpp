/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_large_emoji.h"

#include "main/main_session.h"
#include "chat_helpers/stickers_emoji_pack.h"
#include "history/view/history_view_element.h"
#include "history/history_item.h"
#include "history/history.h"
#include "ui/chat/chat_style.h"
#include "ui/image/image.h"
#include "ui/damage_debug.h"
#include "ui/painter.h"
#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "data/stickers/data_custom_emoji.h"
#include "styles/style_chat.h"

namespace HistoryView {
namespace {

using Stickers::LargeEmojiImage;
using ImagePtr = std::shared_ptr<Stickers::LargeEmojiImage>;
using CustomPtr = std::unique_ptr<Ui::Text::CustomEmoji>;

auto ResolveImages(
	not_null<Main::Session*> session,
	Fn<void(int)> customEmojiRepaint,
	const Ui::Text::IsolatedEmoji &emoji)
-> std::array<LargeEmojiMedia, Ui::Text::kIsolatedEmojiLimit> {
	const auto single = [&](Ui::Text::IsolatedEmoji::Item item, int index)
	-> LargeEmojiMedia {
		if (const auto regular = std::get_if<EmojiPtr>(&item)) {
			return session->emojiStickersPack().image(*regular);
		} else if (const auto custom = std::get_if<QString>(&item)) {
			return session->data().customEmojiManager().create(
				*custom,
				[=] { customEmojiRepaint(index); },
				Data::CustomEmojiManager::SizeTag::Isolated);
		}
		return v::null;
	};
	return { {
		single(emoji.items[0], 0),
		single(emoji.items[1], 1),
		single(emoji.items[2], 2) } };
}

} // namespace

LargeEmoji::LargeEmoji(
	not_null<Element*> parent,
	const Ui::Text::IsolatedEmoji &emoji)
: _parent(parent)
, _images(ResolveImages(
	&parent->history()->session(),
	[=](int index) { repaintCustom(index); },
	emoji)) {
}

LargeEmoji::~LargeEmoji() {
	if (_hasHeavyPart) {
		unloadHeavyPart();
		_parent->checkHeavyPart();
	}
}

QSize LargeEmoji::countOptimalSize() {
	using namespace rpl::mappers;

	resetCustomRepaints();
	const auto count = _images.size()
		- ranges::count(_images, LargeEmojiMedia());

	const auto single = LargeEmojiImage::Size() / style::DevicePixelRatio();
	const auto skip = st::largeEmojiSkip - 2 * st::largeEmojiOutline;
	const auto inner = count * single.width() + (count - 1) * skip;
	const auto &padding = st::largeEmojiPadding;
	_size = QSize(
		padding.left() + inner + padding.right(),
		padding.top() + single.height() + padding.bottom());
	return _size;
}

void LargeEmoji::draw(
		Painter &p,
		const PaintContext &context,
		const QRect &r) {
	_parent->clearCustomEmojiRepaint();

	const auto &padding = st::largeEmojiPadding;
	auto x = r.x() + (r.width() - _size.width()) / 2 + padding.left();
	const auto y = r.y() + (r.height() - _size.height()) / 2 + padding.top();
	const auto skip = st::largeEmojiSkip - 2 * st::largeEmojiOutline;
	const auto size = LargeEmojiImage::Size() / style::DevicePixelRatio();
	const auto selected = context.selected();
	if (!selected) {
		_selectedFrame = QImage();
	}
	for (auto index = 0; index != int(_images.size()); ++index) {
		const auto &media = _images[index];
		if (const auto image = std::get_if<ImagePtr>(&media)) {
			if (const auto &prepared = (*image)->image) {
				const auto colored = selected
					? &context.st->msgStickerOverlay()
					: nullptr;
				p.drawPixmap(
					x,
					y,
					prepared->pix(size, { .colored = colored }));
			} else if ((*image)->load) {
				(*image)->load();
			}
		} else if (const auto custom = std::get_if<CustomPtr>(&media)) {
			paintCustom(p, x, y, index, custom->get(), context);
		} else {
			continue;
		}
		x += size.width() + skip;
	}
}

void LargeEmoji::paintCustom(
		QPainter &p,
		int x,
		int y,
		int index,
		not_null<Ui::Text::CustomEmoji*> emoji,
		const PaintContext &context) {
	if (!_hasHeavyPart) {
		_hasHeavyPart = true;
		_parent->history()->owner().registerHeavyViewPart(_parent);
	}
	const auto inner = st::largeEmojiSize + 2 * st::largeEmojiOutline;
	const auto outer = Ui::Text::AdjustCustomEmojiSize(inner);
	const auto skip = (inner - outer) / 2;
	const auto rect = QRect(x + skip, y + skip, outer, outer);
	//const auto preview = context.imageStyle()->msgServiceBg->c;
	auto &textst = context.st->messageStyle(false, false);
	if (context.selected()) {
		const auto factor = style::DevicePixelRatio();
		const auto size = QSize(outer, outer) * factor;
		if (_selectedFrame.size() != size) {
			_selectedFrame = QImage(
				size,
				QImage::Format_ARGB32_Premultiplied);
			_selectedFrame.setDevicePixelRatio(factor);
		}
		_selectedFrame.fill(Qt::transparent);
		auto q = QPainter(&_selectedFrame);
		const auto repaintBounds = emoji->paint(q, {
			.textColor = textst.historyTextFg->c,
			.now = context.now,
			.paused = context.paused,
		}).repaintBounds();
		q.end();

		_selectedFrame = Images::Colored(
			std::move(_selectedFrame),
			context.st->msgStickerOverlay()->c);
		p.drawImage(rect.topLeft(), _selectedFrame);
		recordCustomFrame(
			p,
			context,
			index,
			repaintBounds.isEmpty()
				? QRectF(rect)
				: repaintBounds.translated(rect.topLeft()));
	} else {
		const auto repaintBounds = emoji->paint(p, {
			.textColor = textst.historyTextFg->c,
			.now = context.now,
			.position = rect.topLeft(),
			.paused = context.paused,
		}).repaintBounds();
		recordCustomFrame(
			p,
			context,
			index,
			repaintBounds.isEmpty() ? QRectF(rect) : repaintBounds);
	}
}

void LargeEmoji::repaintCustom(int index) {
	Expects(index >= 0 && index < int(_customRepaintRects.size()));

	if (_customRepaintPending[index]
		|| (_customRepaintRects[index]
			&& _customRepaintRects[index]->isEmpty())) {
		return;
	}
	_customRepaintPending[index] = true;
	if (_customRepaintRects[index]) {
		_parent->repaint(*_customRepaintRects[index]);
	} else {
		Ui::LogUnknownGeometryRepaint("large emoji");
		_parent->customEmojiRepaint();
	}
}

void LargeEmoji::recordCustomFrame(
		const QPainter &p,
		const PaintContext &context,
		int index,
		QRectF rect) {
	Expects(index >= 0 && index < int(_customRepaintRects.size()));

	if (context.hasElementPainter(p)) {
		const auto mapped = context.mapToElement(p, rect);
		const auto current = mapped.value_or(QRect());
		const auto previous = _customRepaintRects[index].value_or(QRect());
		_customRepaintRects[index] = mapped;
		if (!previous.isEmpty() && previous != current) {
			_customRepaintPending[index] = true;
			_parent->repaint(previous.united(current));
		} else {
			_customRepaintPending[index] = false;
		}
	} else if (!_customRepaintRects[index]
		|| _customRepaintRects[index]->isEmpty()) {
		_customRepaintPending[index] = false;
	}
}

void LargeEmoji::resetCustomRepaints() {
	_customRepaintRects = {};
	_customRepaintPending = {};
}

bool LargeEmoji::hasHeavyPart() const {
	return _hasHeavyPart;
}

void LargeEmoji::unloadHeavyPart() {
	resetCustomRepaints();
	if (_hasHeavyPart) {
		_hasHeavyPart = false;
		for (auto &media : _images) {
			if (const auto custom = std::get_if<CustomPtr>(&media)) {
				(*custom)->unload();
			}
		}
	}
}

} // namespace HistoryView
