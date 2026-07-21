/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/send_files_box_reply_header.h"

#include "chat_helpers/compose/compose_show.h"
#include "core/ui_integration.h"
#include "data/data_changes.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_reply.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/chat/chat_style.h"
#include "ui/effects/spoiler_mess.h"
#include "ui/image/image.h"
#include "ui/paint/damage.h"
#include "ui/painter.h"
#include "ui/text/text_options.h"
#include "ui/power_saving.h"
#include "ui/widgets/buttons.h"
#include "window/window_session_controller.h"
#include "apiwrap.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"

#include <QtGui/QPaintEvent>

namespace SendFiles {
namespace {

constexpr auto kAnimationDuration = crl::time(180);

struct PaintedAnimationDamage final {
	QRegion current;
	QRegion fallback;
};

[[nodiscard]] QRegion MapPaintRegion(const QPainter &p, QRectF rect) {
	return QRegion(Ui::DamageRect(rect, p.transform()));
}

} // namespace

ReplyPillHeader::ReplyPillHeader(
	QWidget *parent,
	std::shared_ptr<ChatHelpers::Show> show,
	FullReplyTo replyTo)
: RpWidget(parent)
, _show(std::move(show))
, _data(&_show->session().data())
, _replyTo(std::move(replyTo))
, _cancel(Ui::CreateChild<Ui::IconButton>(this, st::sendFilesReplyCancel)) {
	resize(
		parent->width(),
		st::boxPhotoCaptionSkip + st::historyReplyHeight);

	_cancel->setAccessibleName(tr::lng_cancel(tr::now));
	_cancel->setClickedCallback([=] {
		hideAnimated();
	});

	setShownMessage(_data->message(_replyTo.messageId));

	_data->session().changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
		| Data::MessageUpdate::Flag::Destroyed
	) | rpl::filter([=](const Data::MessageUpdate &update) {
		return (update.item == _shownMessage);
	}) | rpl::on_next([=](const Data::MessageUpdate &update) {
		if (update.flags & Data::MessageUpdate::Flag::Destroyed) {
			invalidateAnimationDamage(_textAnimationDamage);
			invalidateAnimationDamage(_previewSpoilerDamage);
			_shownMessage = nullptr;
			_shownMessageName.clear();
			_shownMessageText.clear();
			_previewSpoiler = nullptr;
			hideAnimated();
		} else {
			invalidateAnimationDamage(_previewSpoilerDamage);
			updateShownMessageText();
			RpWidget::update();
		}
	}, lifetime());

	animationCallback();
}

ReplyPillHeader::~ReplyPillHeader() = default;

rpl::producer<> ReplyPillHeader::closeRequests() const {
	return _closeRequests.events();
}

rpl::producer<> ReplyPillHeader::hideFinished() const {
	return _hideFinished.value()
		| rpl::filter(rpl::mappers::_1)
		| rpl::to_empty;
}

rpl::producer<int> ReplyPillHeader::desiredHeight() const {
	return _desiredHeight.value();
}

void ReplyPillHeader::setRoundedShapeBelow(bool value) {
	if (_roundedShapeBelow == value) {
		return;
	}
	_roundedShapeBelow = value;
	update();
}

void ReplyPillHeader::hideAnimated() {
	if (_hiding) {
		return;
	}
	_hiding = true;
	_closeRequests.fire({});
	_showAnimation.start(
		[=] { animationCallback(); },
		1.,
		0.,
		kAnimationDuration);
}

void ReplyPillHeader::animationCallback() {
	const auto full = st::boxPhotoCaptionSkip + st::historyReplyHeight;
	const auto value = _showAnimation.value(_hiding ? 0. : 1.);
	_desiredHeight = int(base::SafeRound(full * value));
	update();
	if (_hiding && !_showAnimation.animating()) {
		_hideFinished = true;
	}
}

void ReplyPillHeader::resolveMessageData() {
	const auto id = _replyTo.messageId;
	if (!id || !id.peer) {
		return;
	}
	const auto peer = _data->peer(id.peer);
	const auto itemId = id.msg;
	const auto callback = crl::guard(this, [=] {
		if (!_shownMessage) {
			if (const auto message = _data->message(peer, itemId)) {
				setShownMessage(message);
			} else {
				hideAnimated();
			}
		}
	});
	_data->session().api().requestMessageData(peer, itemId, callback);
}

void ReplyPillHeader::setShownMessage(HistoryItem *item) {
	invalidateAnimationDamage(_textAnimationDamage);
	invalidateAnimationDamage(_previewSpoilerDamage);
	_shownMessage = item;
	if (item) {
		updateShownMessageText();
		const auto context = Core::TextContext({
			.session = &item->history()->session(),
			.customEmojiLoopLimit = 1,
		});
		_shownMessageName.setMarkedText(
			st::fwdTextStyle,
			HistoryView::Reply::ComposePreviewName(
				item->history(),
				item,
				_replyTo),
			Ui::NameTextOptions(),
			context);
	} else {
		_shownMessageName.clear();
		_shownMessageText.clear();
		resolveMessageData();
	}
	update();
}

void ReplyPillHeader::updateShownMessageText() {
	Expects(_shownMessage != nullptr);

	invalidateAnimationDamage(_textAnimationDamage);
	const auto context = Core::TextContext({
		.session = &_data->session(),
		.repaint = [=] { textAnimationRepaint(); },
	});
	_shownMessageText.setMarkedText(
		st::messageTextStyle,
		(_replyTo.quote.empty()
			? _shownMessage->inReplyText()
			: _replyTo.quote),
		Ui::DialogTextOptions(),
		context);
}

void ReplyPillHeader::textAnimationRepaint() {
	if (!_shownMessageText.hasCustomEmoji()
		&& !_shownMessageText.hasSpoilers()) {
		return;
	}
	auto &state = _textAnimationDamage;
	if (state.scheduled) {
		return;
	}
	const auto damage = state.known
		? state.stale.united(state.current)
		: state.stale.united(state.fallback);
	if (damage.isEmpty()) {
		return;
	}
	scheduleAnimationRepaint(state, damage);
}

void ReplyPillHeader::previewSpoilerRepaint() {
	if (!_previewSpoiler) {
		return;
	}
	auto &state = _previewSpoilerDamage;
	if (state.scheduled) {
		return;
	}
	const auto damage = state.known
		? state.stale.united(state.current)
		: state.stale.united(state.fallback);
	if (damage.isEmpty()) {
		return;
	}
	scheduleAnimationRepaint(state, damage);
}

void ReplyPillHeader::invalidateAnimationDamage(AnimationDamage &damage) {
	damage.stale += (damage.known ? damage.current : damage.fallback);
	damage.current = QRegion();
	damage.fallback = QRegion();
	damage.known = false;
}

void ReplyPillHeader::recordAnimationDamage(
		AnimationDamage &state,
		QRegion current,
		QRegion fallback,
		const QRegion &repaintRegion) {
	const auto widgetRegion = QRegion(rect());
	state.current &= widgetRegion;
	state.stale &= widgetRegion;
	state.fallback &= widgetRegion;
	current &= widgetRegion;
	fallback &= widgetRegion;
	fallback += current;
	const auto repainted = repaintRegion.intersected(widgetRegion);
	const auto relevant = state.current
		.united(state.stale)
		.united(state.fallback)
		.united(current)
		.united(fallback);
	if (!repainted.intersects(relevant)) {
		return;
	}
	state.fallback = fallback;
	state.scheduled = false;
	if (!state.known) {
		const auto required = state.stale.united(state.fallback);
		if (!required.subtracted(repainted).isEmpty()) {
			scheduleAnimationRepaint(state, required);
			return;
		}
		state.current = current;
		state.stale = QRegion();
		state.known = true;
		return;
	}
	const auto combined = state.stale
		.united(state.current)
		.united(current);
	if (!combined.subtracted(repainted).isEmpty()) {
		state.current += current;
		scheduleAnimationRepaint(state, combined);
		return;
	}
	state.current = current;
	state.stale = QRegion();
}

void ReplyPillHeader::scheduleAnimationRepaint(
		AnimationDamage &state,
		QRegion damage) {
	if (state.scheduled) {
		return;
	}
	damage &= QRegion(rect());
	if (!damage.isEmpty()) {
		state.scheduled = true;
		update(damage);
	}
}

void ReplyPillHeader::resizeEvent(QResizeEvent *e) {
	invalidateAnimationDamage(_textAnimationDamage);
	invalidateAnimationDamage(_previewSpoilerDamage);
	_cancel->moveToRight(
		st::boxPhotoPadding.right() + st::sendBoxAlbumGroupSkipRight,
		(st::historyReplyHeight - _cancel->height()) / 2);
}

void ReplyPillHeader::paintEvent(QPaintEvent *e) {
	Painter p(this);
	p.setInactive(_show->paused(Window::GifPauseReason::Layer));
	const auto canonical = (p.device() == this);
	const auto repaintRegion = e->region();
	auto textDamage = PaintedAnimationDamage();
	auto previewDamage = PaintedAnimationDamage();
	const auto recordDamage = gsl::finally([&] {
		if (!canonical) {
			return;
		}
		recordAnimationDamage(
			_textAnimationDamage,
			std::move(textDamage.current),
			std::move(textDamage.fallback),
			repaintRegion);
		recordAnimationDamage(
			_previewSpoilerDamage,
			std::move(previewDamage.current),
			std::move(previewDamage.fallback),
			repaintRegion);
	});

	const auto left = st::boxPhotoPadding.left();
	const auto right = st::boxPhotoPadding.right();
	const auto bottomSkip = st::boxPhotoCaptionSkip;
	const auto pillHeight = height() - bottomSkip;
	if (pillHeight <= 0) {
		return;
	}
	const auto pillRect = QRect(
		left,
		0,
		width() - left - right,
		pillHeight);
	if (pillRect.isEmpty()) {
		return;
	}

	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		const auto topRadius = st::bubbleRadiusLarge;
		const auto bottomRadius = _roundedShapeBelow
			? st::bubbleRadiusSmall
			: st::bubbleRadiusLarge;
		const auto rectF = QRectF(pillRect);
		auto path = QPainterPath();
		path.moveTo(rectF.left() + topRadius, rectF.top());
		path.lineTo(rectF.right() - topRadius, rectF.top());
		path.arcTo(
			rectF.right() - 2 * topRadius,
			rectF.top(),
			2 * topRadius,
			2 * topRadius,
			90, -90);
		path.lineTo(rectF.right(), rectF.bottom() - bottomRadius);
		path.arcTo(
			rectF.right() - 2 * bottomRadius,
			rectF.bottom() - 2 * bottomRadius,
			2 * bottomRadius,
			2 * bottomRadius,
			0, -90);
		path.lineTo(rectF.left() + bottomRadius, rectF.bottom());
		path.arcTo(
			rectF.left(),
			rectF.bottom() - 2 * bottomRadius,
			2 * bottomRadius,
			2 * bottomRadius,
			270, -90);
		path.lineTo(rectF.left(), rectF.top() + topRadius);
		path.arcTo(
			rectF.left(),
			rectF.top(),
			2 * topRadius,
			2 * topRadius,
			180, -90);
		path.closeSubpath();
		p.fillPath(path, st::windowBgOver);
	}

	const auto iconPos = st::sendFilesReplyIconPosition
		+ QPoint(pillRect.left(), pillRect.top());
	if (!_replyTo.quote.empty()) {
		st::historyQuoteIcon.paint(p, iconPos, width());
	} else {
		st::historyReplyIcon.paint(p, iconPos, width());
		// Remove 'settings' mini-icon.
		p.fillRect(
			QRect(
				QPoint(style::ConvertScale(16), style::ConvertScale(5))
					+ iconPos,
				QSize(style::ConvertScale(11), style::ConvertScale(8))),
			st::windowBgOver);
		p.fillRect(
			QRect(
				QPoint(style::ConvertScale(22), style::ConvertScale(13))
					+ iconPos,
				QSize(style::ConvertScale(5), style::ConvertScale(2))),
			st::windowBgOver);
	}

	const auto replySkip = st::historyReplySkip;
	const auto textLeft = pillRect.left() + replySkip;
	const auto availableWidth = _cancel->x() - textLeft;
	if (availableWidth <= 0) {
		return;
	}

	const auto pillCenterY = pillRect.top()
		+ st::historyReplyHeight / 2;

	if (!_shownMessage) {
		p.setFont(st::msgDateFont);
		p.setPen(st::historyComposeAreaFgService);
		const auto top = pillCenterY - st::msgDateFont->height / 2;
		p.drawText(
			textLeft,
			top + st::msgDateFont->ascent,
			st::msgDateFont->elided(
				tr::lng_profile_loading(tr::now),
				availableWidth));
		return;
	}

	const auto media = _shownMessage->media();
	const auto hasPreview = media && media->hasReplyPreview();
	const auto preview = hasPreview ? media->replyPreview() : nullptr;
	const auto spoilered = media && media->hasSpoiler();
	if (!spoilered) {
		_previewSpoiler = nullptr;
	} else if (!_previewSpoiler) {
		_previewSpoiler = std::make_unique<Ui::SpoilerAnimation>([=] {
			previewSpoilerRepaint();
		});
	}
	const auto previewSkipValue = st::historyReplyPreview + st::msgReplyBarSkip;
	const auto previewSkip = (hasPreview && preview) ? previewSkipValue : 0;
	const auto contentLeft = textLeft + previewSkip;
	const auto contentAvailable = availableWidth - previewSkip;
	const auto previewRect = hasPreview
		? QRect(
			textLeft,
			pillCenterY - st::historyReplyPreview / 2,
			st::historyReplyPreview,
			st::historyReplyPreview)
		: QRect();
	if (canonical && !previewRect.isEmpty()) {
		previewDamage.fallback = MapPaintRegion(p, previewRect);
	}

	if (preview) {
		p.drawPixmap(previewRect.x(), previewRect.y(), preview->pixSingle(
			preview->size() / style::DevicePixelRatio(),
			{
				.options = Images::Option::RoundSmall,
				.outer = previewRect.size(),
			}));
		if (_previewSpoiler) {
			Ui::FillSpoilerRect(
				p,
				previewRect,
				Ui::DefaultImageSpoiler().frame(
					_previewSpoiler->index(crl::now(), p.inactive())));
			if (canonical) {
				previewDamage.current = previewDamage.fallback;
			}
		}
	}

	p.setPen(st::historyReplyNameFg);
	p.setFont(st::msgServiceNameFont);
	_shownMessageName.drawElided(
		p,
		contentLeft,
		pillRect.top() + st::msgReplyPadding.top(),
		contentAvailable);

	p.setPen(st::historyComposeAreaFg);
	const auto textPosition = QPoint(
		contentLeft,
		pillRect.top()
			+ st::msgReplyPadding.top()
			+ st::msgServiceNameFont->height);
	auto customEmojiRepaintBounds = Ui::Text::CustomEmojiRepaintBounds();
	if (canonical) {
		textDamage.fallback = MapPaintRegion(p, QRectF(
			textPosition,
			QSize(
				std::max(contentAvailable, 0),
				_shownMessageText.lineHeight())));
	}
	_shownMessageText.draw(p, {
		.position = textPosition,
		.availableWidth = contentAvailable,
		.palette = &st::historyComposeAreaPalette,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = crl::now(),
		.pausedEmoji = p.inactive() || On(PowerSaving::kEmojiChat),
		.pausedSpoiler = p.inactive() || On(PowerSaving::kChatSpoiler),
		.elisionLines = 1,
		.customEmojiRepaintBounds = canonical
			? &customEmojiRepaintBounds
			: nullptr,
	});
	if (canonical) {
		const auto hasCustomEmoji = _shownMessageText.hasCustomEmoji();
		if (_shownMessageText.hasSpoilers()
			|| (hasCustomEmoji
				&& !customEmojiRepaintBounds.repaintBoundsKnown)) {
			textDamage.current += textDamage.fallback;
		}
		if (hasCustomEmoji) {
			textDamage.current += MapPaintRegion(
				p,
				customEmojiRepaintBounds.rect);
		}
	}
}

} // namespace SendFiles
