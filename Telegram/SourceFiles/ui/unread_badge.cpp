/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/unread_badge.h"

#include "data/data_emoji_statuses.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "main/main_session.h"
#include "lang/lang_keys.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/power_saving.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/unread_badge_paint.h"
#include "styles/style_dialogs.h"

namespace Ui {
namespace {

constexpr auto kPlayStatusLimit = 2;
constexpr auto kBotVerifiedScale = 0.88;

struct ScaledBotVerifiedEmojiCache {
	QImage frame;
	QColor frameColor;
	uint64 version = 0;
};

class ScaledBotVerifiedEmoji final : public Ui::Text::CustomEmoji {
public:
	ScaledBotVerifiedEmoji(
		std::unique_ptr<Ui::Text::CustomEmoji> wrapped,
		int innerSize,
		int outerSize,
		std::shared_ptr<ScaledBotVerifiedEmojiCache> cache);

	int width() override;
	QString entityData() override;
	PaintResult paint(QPainter &p, const Context &context) override;
	void unload() override;
	bool ready() override;
	bool readyInDefaultState() override;

private:
	const std::unique_ptr<Ui::Text::CustomEmoji> _wrapped;
	const int _innerSize = 0;
	const int _outerSize = 0;
	const std::shared_ptr<ScaledBotVerifiedEmojiCache> _cache;

};

ScaledBotVerifiedEmoji::ScaledBotVerifiedEmoji(
	std::unique_ptr<Ui::Text::CustomEmoji> wrapped,
	int innerSize,
	int outerSize,
	std::shared_ptr<ScaledBotVerifiedEmojiCache> cache)
: _wrapped(std::move(wrapped))
, _innerSize(innerSize)
, _outerSize(outerSize)
, _cache(std::move(cache)) {
}

int ScaledBotVerifiedEmoji::width() {
	return _outerSize;
}

QString ScaledBotVerifiedEmoji::entityData() {
	return _wrapped->entityData();
}

Ui::Text::CustomEmoji::PaintResult ScaledBotVerifiedEmoji::paint(
		QPainter &p,
		const Context &context) {
	const auto skip = (_outerSize - _innerSize) / 2;
	const auto position = context.position + QPoint(skip, skip);
	const auto repaintBounds = QRectF(
		position,
		QSize(_innerSize, _innerSize));
	if (_cache->frame.isNull() || _cache->frameColor != context.textColor) {
		if (!_wrapped->ready()) {
			return PaintResult(QRectF(), repaintBounds);
		}
		const auto ratio = style::DevicePixelRatio();
		const auto sourcePx = Data::FrameSizeFromTag(
			Data::CustomEmojiSizeTag::Isolated);
		auto frame = QImage(
			QSize(sourcePx, sourcePx),
			QImage::Format_ARGB32_Premultiplied);
		frame.setDevicePixelRatio(ratio);
		frame.fill(Qt::transparent);

		const auto version = _cache->version;
		auto painter = QPainter(&frame);
		painter.translate(-context.position);
		const auto was = context.internal.forceFirstFrame;
		context.internal.forceFirstFrame = true;
		const auto painted = _wrapped->paint(
			painter,
			context).paintedBounds();
		context.internal.forceFirstFrame = was;
		const auto sourceBounds = painted.isEmpty()
			? QRectF()
			: painter.transform().map(
				QPolygonF(painted)
			).boundingRect().intersected(
				QRectF(
					QPointF(),
					QSize(sourcePx / ratio, sourcePx / ratio)));
		painter.end();
		if (sourceBounds.isEmpty()) {
			return PaintResult(QRectF(), repaintBounds);
		}

		frame = frame.scaled(
			QSize(_innerSize, _innerSize) * ratio,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
		if (_cache->version != version) {
			return PaintResult(QRectF(), repaintBounds);
		}
		_cache->frame = std::move(frame);
		_cache->frameColor = context.textColor;
	}
	p.drawImage(position, _cache->frame);
	return PaintResult(
		QRectF(position, QSize(_innerSize, _innerSize)),
		repaintBounds);
}

void ScaledBotVerifiedEmoji::unload() {
	_cache->frame = QImage();
	++_cache->version;
	_wrapped->unload();
}

bool ScaledBotVerifiedEmoji::ready() {
	return !_cache->frame.isNull() || _wrapped->ready();
}

bool ScaledBotVerifiedEmoji::readyInDefaultState() {
	return !_cache->frame.isNull() || _wrapped->ready();
}

} // namespace

struct PeerBadge::EmojiStatus {
	EmojiStatusId id;
	std::unique_ptr<Ui::Text::CustomEmoji> emoji;
	QPoint lastPosition;
	QRect lastRect;
	QColor lastColor;
	int skip = 0;
};

struct PeerBadge::BotVerifiedData {
	std::unique_ptr<Text::CustomEmoji> icon;
	QPoint lastOrigin;
	QRect lastRect;
};

void UnreadBadge::setText(const QString &text, bool active) {
	_text = text;
	_active = active;
	const auto st = Dialogs::Ui::UnreadBadgeStyle();
	resize(
		std::max(st.font->width(text) + 2 * st.padding, st.size),
		st.size);
	update();
}

int UnreadBadge::textBaseline() const {
	const auto st = Dialogs::Ui::UnreadBadgeStyle();
	return ((st.size - st.font->height) / 2) + st.font->ascent;
}

void UnreadBadge::paintEvent(QPaintEvent *e) {
	if (_text.isEmpty()) {
		return;
	}

	auto p = QPainter(this);

	UnreadBadgeStyle unreadSt;
	unreadSt.muted = !_active;
	auto unreadRight = width();
	auto unreadTop = 0;
	PaintUnreadBadge(
		p,
		_text,
		unreadRight,
		unreadTop,
		unreadSt);
}

QString TextBadgeText(TextBadgeType type) {
	switch (type) {
	case TextBadgeType::Fake: return tr::lng_fake_badge(tr::now);
	case TextBadgeType::Scam: return tr::lng_scam_badge(tr::now);
	case TextBadgeType::Direct: return tr::lng_direct_badge(tr::now);
	}
	Unexpected("Type in TextBadgeText.");
}

QSize TextBadgeSize(TextBadgeType type) {
	const auto phrase = TextBadgeText(type);
	const auto phraseWidth = st::dialogsScamFont->width(phrase);
	const auto width = st::dialogsScamPadding.left()
		+ phraseWidth
		+ st::dialogsScamPadding.right();
	const auto height = st::dialogsScamPadding.top()
		+ st::dialogsScamFont->height
		+ st::dialogsScamPadding.bottom();
	return { width, height };
}

void DrawTextBadge(
		Painter &p,
		QRect rect,
		int outerWidth,
		const style::color &color,
		const QString &phrase,
		int phraseWidth) {
	PainterHighQualityEnabler hq(p);
	auto pen = color->p;
	pen.setWidth(st::lineWidth);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(rect, st::dialogsScamRadius, st::dialogsScamRadius);
	p.setFont(st::dialogsScamFont);
	if (style::DevicePixelRatio() > 1) {
		p.drawText(
			QRect(
				rect.x() + st::dialogsScamPadding.left(),
				rect.y() + st::dialogsScamPadding.top(),
				rect.width() - rect::m::sum::h(st::dialogsScamPadding),
				rect.height() - rect::m::sum::v(st::dialogsScamPadding)),
			Qt::AlignCenter,
			phrase);
	} else {
		p.drawTextLeft(
			rect.x() + st::dialogsScamPadding.left(),
			rect.y() + st::dialogsScamPadding.top(),
			outerWidth,
			phrase,
			phraseWidth);
	}
}

void DrawTextBadge(
		TextBadgeType type,
		Painter &p,
		QRect rect,
		int outerWidth,
		const style::color &color) {
	const auto phrase = TextBadgeText(type);
	DrawTextBadge(
		p,
		rect,
		outerWidth,
		color,
		phrase,
		st::dialogsScamFont->width(phrase));
}

PeerBadge::PeerBadge() = default;

PeerBadge::~PeerBadge() = default;

int PeerBadge::drawGetWidth(Painter &p, Descriptor &&descriptor) {
	Expects(descriptor.customEmojiRepaint != nullptr);

	const auto peer = descriptor.peer;
	if ((descriptor.scam && (peer->isScam() || peer->isFake()))
		|| (descriptor.direct && peer->isMonoforum())) {
		_emojiStatus = nullptr;
		return drawTextBadge(p, descriptor);
	}
	const auto verifyCheck = descriptor.verified && peer->isVerified();
	const auto premiumMark = descriptor.premium
		&& peer->session().premiumBadgesShown();
	const auto emojiStatus = premiumMark
		&& peer->emojiStatusId()
		&& (peer->isPremium() || peer->isChannel());
	const auto premiumStar = premiumMark
		&& !emojiStatus
		&& peer->isPremium();

	const auto paintVerify = verifyCheck
		&& (descriptor.prioritizeVerification
			|| descriptor.bothVerifyAndStatus
			|| !emojiStatus);
	const auto paintEmoji = emojiStatus
		&& (!paintVerify || descriptor.bothVerifyAndStatus);
	const auto paintStar = premiumStar && !paintVerify;
	if (!paintEmoji) {
		_emojiStatus = nullptr;
	}

	auto result = 0;
	if (paintEmoji) {
		auto &rectForName = descriptor.rectForName;
		const auto verifyWidth = descriptor.verified->width();
		if (paintVerify) {
			rectForName.setWidth(rectForName.width() - verifyWidth);
		}
		result += drawPremiumEmojiStatus(p, descriptor);
		if (!paintVerify) {
			return result;
		}
		rectForName.setWidth(rectForName.width() + verifyWidth);
		descriptor.nameWidth += result;
	}
	if (paintVerify) {
		result += drawVerifyCheck(p, descriptor);
		return result;
	} else if (paintStar) {
		return drawPremiumStar(p, descriptor);
	}
	return 0;
}

int PeerBadge::drawTextBadge(Painter &p, const Descriptor &descriptor) {
	const auto type = [&] {
		if (descriptor.peer->isScam()) {
			return TextBadgeType::Scam;
		} else if (descriptor.peer->isFake()) {
			return TextBadgeType::Fake;
		}
		return TextBadgeType::Direct;
	}();
	const auto phrase = TextBadgeText(type);
	const auto phraseWidth = st::dialogsScamFont->width(phrase);
	const auto width = st::dialogsScamPadding.left()
		+ phraseWidth
		+ st::dialogsScamPadding.right();
	const auto height = st::dialogsScamPadding.top()
		+ st::dialogsScamFont->height
		+ st::dialogsScamPadding.bottom();
	const auto rectForName = descriptor.rectForName;
	const auto rect = QRect(
		(rectForName.x()
			+ qMin(
				descriptor.nameWidth + st::dialogsScamSkip,
				rectForName.width() - width)),
		rectForName.y() + (rectForName.height() - height) / 2,
		width,
		height);
	DrawTextBadge(
		p,
		rect,
		descriptor.outerWidth,
		*((type == TextBadgeType::Direct)
			? descriptor.direct
			: descriptor.scam),
		phrase,
		phraseWidth);
	return st::dialogsScamSkip + width;
}

int PeerBadge::drawVerifyCheck(Painter &p, const Descriptor &descriptor) {
	const auto iconw = descriptor.verified->width();
	const auto rectForName = descriptor.rectForName;
	const auto nameWidth = descriptor.nameWidth;
	descriptor.verified->paint(
		p,
		rectForName.x() + qMin(nameWidth, rectForName.width() - iconw),
		rectForName.y(),
		descriptor.outerWidth);
	return iconw;
}

int PeerBadge::drawPremiumEmojiStatus(
		Painter &p,
		const Descriptor &descriptor) {
	const auto peer = descriptor.peer;
	const auto id = peer->emojiStatusId();
	const auto rectForName = descriptor.rectForName;
	if (!_emojiStatus) {
		_emojiStatus = std::make_unique<EmojiStatus>();
		const auto size = st::emojiSize;
		const auto emoji = Ui::Text::AdjustCustomEmojiSize(size);
		_emojiStatus->skip = (size - emoji) / 2;
	}
	const auto emojiSize = Ui::Text::AdjustCustomEmojiSize(st::emojiSize);
	const auto width = emojiSize - 2 * _emojiStatus->skip;
	const auto iconx = rectForName.x()
		+ qMin(descriptor.nameWidth, rectForName.width() - width);
	const auto icony = rectForName.y();
	_emojiStatus->lastPosition = QPoint(
		iconx - 2 * _emojiStatus->skip,
		icony + _emojiStatus->skip);
	_emojiStatus->lastColor = (*descriptor.premiumFg)->c;
	_emojiStatus->lastRect = QRect();
	if (_emojiStatus->id != id) {
		using namespace Ui::Text;
		auto &manager = peer->session().data().customEmojiManager();
		_emojiStatus->id = id;
		_emojiStatus->emoji = MakeWrappedEmoji<LimitedLoopsEmoji>(
			manager.create(
				Data::EmojiStatusCustomId(id),
				descriptor.customEmojiRepaint),
			kPlayStatusLimit);
	}
	if (!_emojiStatus->emoji) {
		_emojiStatus->lastRect = QRect();
		return 0;
	}
	const auto repaintBounds = _emojiStatus->emoji->paint(p, {
		.textColor = _emojiStatus->lastColor,
		.now = descriptor.now,
		.position = _emojiStatus->lastPosition,
		.paused = descriptor.paused || On(PowerSaving::kEmojiStatus),
	}).repaintBounds();
	if (!repaintBounds.isEmpty()) {
		_emojiStatus->lastRect = repaintBounds.toAlignedRect();
	}
	return width;
}

int PeerBadge::drawPremiumStar(Painter &p, const Descriptor &descriptor) {
	const auto rectForName = descriptor.rectForName;
	const auto iconw = descriptor.premium->width();
	const auto iconx = rectForName.x()
		+ qMin(descriptor.nameWidth, rectForName.width() - iconw);
	const auto icony = rectForName.y();
	_emojiStatus = nullptr;
	descriptor.premium->paint(p, iconx, icony, descriptor.outerWidth);
	return iconw;
}

QRect PeerBadge::emojiStatusRect() const {
	if (!_emojiStatus) {
		return QRect();
	}
	return _emojiStatus->lastRect;
}

QRect PeerBadge::botVerifiedRect() const {
	return _botVerifiedData ? _botVerifiedData->lastRect : QRect();
}

void PeerBadge::paintBotVerifiedFrame(
		QPainter &p,
		crl::time now,
		const style::VerifiedBadge &st) {
	const auto data = _botVerifiedData.get();
	if (!data || !data->icon) {
		return;
	}
	const auto position = data->lastOrigin + st.position;
	const auto repaintBounds = data->icon->paint(p, {
		.textColor = st.color->c,
		.now = now,
		.position = position,
	}).repaintBounds();
	if (!repaintBounds.isEmpty()) {
		data->lastRect = repaintBounds.toAlignedRect();
	}
}

void PeerBadge::paintEmojiStatusFrame(
		QPainter &p,
		crl::time now,
		bool paused) {
	if (!_emojiStatus || !_emojiStatus->emoji) {
		return;
	}
	paintEmojiStatusFrame(p, now, paused, _emojiStatus->lastPosition);
}

void PeerBadge::paintEmojiStatusFrame(
		QPainter &p,
		crl::time now,
		bool paused,
		QPoint position) {
	if (!_emojiStatus || !_emojiStatus->emoji) {
		return;
	}
	_emojiStatus->emoji->paint(p, {
		.textColor = _emojiStatus->lastColor,
		.now = now,
		.position = position,
		.paused = paused || On(PowerSaving::kEmojiStatus),
	});
}

void PeerBadge::unload() {
	_emojiStatus = nullptr;
	_botVerifiedData = nullptr;
}

bool PeerBadge::ready(const BotVerifyDetails *details) const {
	if (!details || !*details) {
		_botVerifiedData = nullptr;
		return true;
	} else if (!_botVerifiedData) {
		return false;
	}
	if (!details->iconId) {
		_botVerifiedData->icon = nullptr;
		_botVerifiedData->lastRect = QRect();
	} else if (!_botVerifiedData->icon
		|| (_botVerifiedData->icon->entityData()
			!= Data::SerializeCustomEmojiId(details->iconId))) {
		_botVerifiedData->lastRect = QRect();
		return false;
	}
	return true;
}

void PeerBadge::set(
		not_null<const BotVerifyDetails*> details,
		Ui::Text::CustomEmojiFactory factory,
		Fn<void()> repaint) {
	if (!_botVerifiedData) {
		_botVerifiedData = std::make_unique<BotVerifiedData>();
	}
	_botVerifiedData->icon = nullptr;
	_botVerifiedData->lastRect = QRect();
	if (details->iconId) {
		const auto outer = st::emojiSize;
		const auto inner = int(base::SafeRound(
			st::emojiSize * kBotVerifiedScale));
		const auto cache = std::make_shared<ScaledBotVerifiedEmojiCache>();
		_botVerifiedData->icon = MakeWrappedEmoji<ScaledBotVerifiedEmoji>(
			factory(
				Data::SerializeCustomEmojiId(details->iconId),
				{ .repaint = [cache, repaint = std::move(repaint)] {
					cache->frame = QImage();
					++cache->version;
					if (repaint) {
						repaint();
					}
				} }),
			inner,
			outer,
			cache);
	}
}

int PeerBadge::drawVerified(
		QPainter &p,
		QPoint position,
		const style::VerifiedBadge &st) {
	const auto data = _botVerifiedData.get();
	if (!data) {
		return 0;
	}
	data->lastRect = QRect();
	if (const auto icon = data->icon.get()) {
		data->lastOrigin = position;
		paintBotVerifiedFrame(p, crl::now(), st);
		return icon->width();
	}
	return 0;
}

} // namespace Ui
