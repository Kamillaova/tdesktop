/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_summary_header.h"

#include "api/api_transcribes.h"
#include "apiwrap.h"
#include "core/click_handler_types.h"
#include "core/ui_integration.h"
#include "data/data_session.h"
#include "history/history_item_components.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/about_cocoon_box.h"
#include "ui/chat/chat_style.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/damage_debug.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/rect.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"

namespace HistoryView {

SummaryHeader::SummaryHeader()
: _name(st::maxSignatureSize / 2)
, _text(st::maxSignatureSize / 2) {
}

SummaryHeader &SummaryHeader::operator=(SummaryHeader &&other) = default;

SummaryHeader::~SummaryHeader() = default;

void SummaryHeader::update(not_null<Element*> view) {
	const auto item = view->data();

	if (!_animation) {
		ensureAnimation();
	}
	if (!_lottie) {
		ensureLottie();
	}

	_text.setText(
		st::defaultTextStyle,
		tr::lng_summarize_header_about(tr::now));

	_name.setText(
		st::msgNameStyle,
		tr::lng_summarize_header_title(tr::now));

	_maxWidth = st::historyReplyPadding.left()
		+ st::maxSignatureSize / 2
		+ st::historyReplyPadding.right();

	const auto session = &item->history()->session();
	const auto itemId = item->fullId();
	_link = std::make_shared<LambdaClickHandler>([=](ClickContext context) {
		if (Rect(iconRect().size()).contains(_iconRipple.lastPoint)) {
			const auto my = context.other.value<ClickHandlerContext>();
			if (const auto controller = my.sessionWindow.get()) {
				controller->show(Box(Ui::AboutCocoonBox));
				return;
			}
		}
		if (const auto item = session->data().message(itemId)) {
			session->api().transcribes().toggleSummary(item);
		}
	});
}

bool SummaryHeader::isNameUpdated(not_null<const Element*> view) const {
	return false;
}

int SummaryHeader::resizeToWidth(int width) const {
	const auto height = st::historyReplyPadding.top()
		+ st::msgServiceNameFont->height * 2
		+ st::historyReplyPadding.bottom();
	if (_width != width || _height != height) {
		invalidateRippleRepaint(_ripple.repaint);
		invalidateRippleRepaint(_iconRipple.repaint);
	}
	_ripple.animation = nullptr;
	++_ripple.repaint.generation;
	_height = height;
	_width = width;
	return _height;
}

int SummaryHeader::height() const {
	return _height + st::historyReplyTop + st::historyReplyBottom;
}

QMargins SummaryHeader::margins() const {
	return QMargins(0, st::historyReplyTop, 0, st::historyReplyBottom);
}

void SummaryHeader::paint(
		Painter &p,
		not_null<const Element*> view,
		const Ui::ChatPaintContext &context,
		int x,
		int y,
		int w,
		bool inBubble) const {
	const auto st = context.st;
	const auto stm = context.messageStyle();

	y += st::historyReplyTop;
	const auto rect = QRect(x, y, w, _height);
	recordRippleRepaint(view, _ripple.repaint, p, context, rect);
	const auto colorPattern = 0;
	const auto cache = !inBubble
		? st->serviceReplyCache(colorPattern).get()
		: stm->replyCache[colorPattern].get();
	const auto &quoteSt = st::messageQuoteStyle;
	const auto rippleColor = cache->bg;
	const auto nameColor = !inBubble
		? st->msgImgReplyBarColor()->c
		: stm->msgServiceFg->c;
	if (!inBubble) {
		cache->bg = QColor(0, 0, 0, 0);
	}
	Ui::Text::ValidateQuotePaintCache(*cache, quoteSt);
	Ui::Text::FillQuotePaint(p, rect, *cache, quoteSt);
	if (!inBubble) {
		cache->bg = rippleColor;
	}

	if (!_lottie) {
		ensureLottie();
	}
	{
		const auto r = iconRect().translated(x, y);
		recordRippleRepaint(view, _iconRipple.repaint, p, context, r);
		const auto lottieX = r.x() + st::historySummaryHeaderIconSizeInner;
		const auto lottieY = r.y() + st::historySummaryHeaderIconSizeInner;
		_lottie->paint(p, lottieX, lottieY, nameColor);
		if (_iconRipple.animation) {
			p.save();
			p.translate(r.topLeft());
			_iconRipple.animation->paint(
				p,
				0,
				0,
				r.width(),
				&rippleColor);
			p.restore();
			if (_iconRipple.animation->empty()) {
				_iconRipple.animation.reset();
			}
		}
	}

	if (!_animation) {
		ensureAnimation();
	}
	{
		const auto size = QSize(w, _height);
		const auto particlesRect = QRect(QPoint(), size);
		if (_animation->cachedSize != size) {
			_animation->path = QPainterPath();
			_animation->path.addRoundedRect(
				particlesRect,
				quoteSt.radius,
				quoteSt.radius);
			_animation->cachedSize = size;
		}
		p.translate(x, y);
		p.setClipPath(_animation->path);
		_animation->particles.setColor(nameColor);
		const auto paused = context.paused || On(PowerSaving::kChatEffects);
		_animation->particles.paint(
			p,
			particlesRect,
			context.now,
			paused);
		if (!paused) {
			scheduleParticlesRepaint(
				view,
				context.mapToElement(
					p,
					QRectF(particlesRect)));
		}
		p.setClipping(false);
		p.translate(-x, -y);
	}

	auto textLeft = x + st::historyReplyPadding.left();
	auto textTop = y + st::historyReplyPadding.top()
		+ st::msgServiceNameFont->height;
	if (w > st::historyReplyPadding.left()) {
		const auto iconSpace = st::historySummaryHeaderIconSize
			+ st::historySummaryHeaderIconSizeInner * 2;
		const auto textw = w
			- st::historyReplyPadding.left()
			- st::historyReplyPadding.right()
			- iconSpace;
		const auto namew = textw;
		if (namew > 0) {
			p.setPen(nameColor);
			_name.drawLeftElided(
				p,
				x + st::historyReplyPadding.left(),
				y + st::historyReplyPadding.top(),
				namew,
				w + 2 * x,
				1);

			p.setPen(inBubble
				? stm->historyTextFg
				: st->msgImgReplyBarColor());
			view->prepareCustomEmojiPaint(p, context, _text);
			auto replyToTextPalette = &(!inBubble
				? st->imgReplyTextPalette()
				: stm->replyTextPalette);
			_text.draw(p, {
				.position = { textLeft, textTop },
				.availableWidth = textw,
				.palette = replyToTextPalette,
				.spoiler = Ui::Text::DefaultSpoilerCache(),
				.now = context.now,
				.pausedEmoji = (context.paused
					|| On(PowerSaving::kEmojiChat)),
				.pausedSpoiler = (context.paused
					|| On(PowerSaving::kChatSpoiler)),
				.elisionLines = 1,
			});
			p.setTextPalette(stm->textPalette);
		}
	}

	if (_ripple.animation) {
		p.save();
		p.translate(rect.topLeft());
		_ripple.animation->paint(
			p,
			0,
			0,
			rect.width(),
			&rippleColor);
		p.restore();
		if (_ripple.animation->empty()) {
			_ripple.animation.reset();
		}
	}
}

void SummaryHeader::scheduleParticlesRepaint(
		not_null<const Element*> view,
		std::optional<QRect> rect) const {
	if (rect && rect->isEmpty()) {
		return;
	}
	const auto alreadyPending = (_particlesRepaintState
		!= ParticlesRepaintState::None);
	if (!rect) {
		_particlesRepaintRect = QRect();
		_particlesRepaintState = ParticlesRepaintState::Full;
	} else if (_particlesRepaintState != ParticlesRepaintState::Full) {
		_particlesRepaintRect = _particlesRepaintRect.united(*rect);
		_particlesRepaintState = ParticlesRepaintState::Rect;
	}
	if (alreadyPending) {
		return;
	}
	const auto weak = base::make_weak(view);
	Ui::PostponeCall(&view->history()->session(), [weak] {
		if (const auto strong = weak.get()) {
			if (const auto header = strong->Get<SummaryHeader>()) {
				header->repaintParticles(strong);
			}
		}
	});
}

void SummaryHeader::repaintParticles(
		not_null<const Element*> view) const {
	const auto state = base::take(_particlesRepaintState);
	const auto rect = base::take(_particlesRepaintRect);
	if (state == ParticlesRepaintState::Full) {
		Ui::LogUnknownGeometryRepaint("summary header particles");
		view->repaint();
	} else if (state == ParticlesRepaintState::Rect) {
		view->repaint(rect);
	}
}

void SummaryHeader::createRippleAnimation(
		not_null<const Element*> view,
		QSize size) {
	const auto weak = base::make_weak(view);
	const auto rippleGeneration = ++_ripple.repaint.generation;
	_ripple.animation = std::make_unique<Ui::RippleAnimation>(
		st::defaultRippleAnimation,
		Ui::RippleAnimation::RoundRectMask(
			size,
			st::messageQuoteStyle.radius),
		[weak, rippleGeneration] {
			if (const auto strong = weak.get()) {
				if (const auto header = strong->Get<SummaryHeader>()) {
					header->repaintRipple(
						strong,
						header->_ripple.repaint,
						rippleGeneration);
				}
			}
		});
	const auto rippleIconSize = st::historySummaryHeaderIconSize;
	const auto iconRippleGeneration = ++_iconRipple.repaint.generation;
	_iconRipple.animation = std::make_unique<Ui::RippleAnimation>(
		st::defaultRippleAnimation,
		Ui::RippleAnimation::EllipseMask(Size(rippleIconSize)),
		[weak, iconRippleGeneration] {
			if (const auto strong = weak.get()) {
				if (const auto header = strong->Get<SummaryHeader>()) {
					header->repaintRipple(
						strong,
						header->_iconRipple.repaint,
						iconRippleGeneration);
				}
			}
		});
}

void SummaryHeader::repaintRipple(
		not_null<const Element*> view,
		RippleRepaint &repaint,
		uint64 generation) const {
	if (repaint.generation != generation || repaint.pending) {
		return;
	} else if (repaint.known && repaint.current.isEmpty()) {
		return;
	}
	repaint.pending = true;
	if (!repaint.known) {
		Ui::LogUnknownGeometryRepaint("summary header ripple");
		view->repaint();
	} else {
		repaintRippleRegion(view, repaint.current);
	}
}

void SummaryHeader::recordRippleRepaint(
		not_null<const Element*> view,
		RippleRepaint &repaint,
		const Painter &p,
		const Ui::ChatPaintContext &context,
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
			current += *mapped;
		}
	}
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.current = known ? std::move(current) : QRegion();
	repaint.known = known;
	if (!known) {
		repaint.stale = previous;
		if (!previous.isEmpty()) {
			repaint.pending = true;
			Ui::LogUnknownGeometryRepaint("summary header ripple");
			view->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = true;
	repaintRippleRegion(view, previous.united(repaint.current));
}

void SummaryHeader::invalidateRippleRepaint(
		RippleRepaint &repaint) const {
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.known = false;
}

void SummaryHeader::repaintRippleRegion(
		not_null<const Element*> view,
		const QRegion &region) const {
	view->repaint(region);
}

void SummaryHeader::saveRipplePoint(QPoint point) const {
	_ripple.lastPoint = point;
	const auto rect = iconRect();
	if (rect.contains(point)) {
		_iconRipple.lastPoint = point - rect.topLeft();
	} else {
		_iconRipple.lastPoint = QPoint(-1, -1);
	}
}

void SummaryHeader::addRipple() {
	if (_iconRipple.lastPoint.x() >= 0 && _iconRipple.animation) {
		_iconRipple.animation->add(_iconRipple.lastPoint);
	} else if (_ripple.animation) {
		_ripple.animation->add(_ripple.lastPoint);
	}
}

void SummaryHeader::stopLastRipple() {
	if (_ripple.animation) {
		_ripple.animation->lastStop();
	}
	if (_iconRipple.animation) {
		_iconRipple.animation->lastStop();
	}
}

void SummaryHeader::unloadHeavyPart() {
	_unloadTime = crl::now();
	_animation = nullptr;
	_ripple.animation = nullptr;
	++_ripple.repaint.generation;
	_iconRipple.animation = nullptr;
	++_iconRipple.repaint.generation;
	_lottie = nullptr;
}

QRect SummaryHeader::iconRect() const {
	const auto size = st::historySummaryHeaderIconSize;
	const auto shift = st::historySummaryHeaderIconSizeInner;
	return QRect(_width - size - shift, (_height - size) / 2, size, size);
}

void SummaryHeader::ensureAnimation() const {
	using namespace Ui;
	_animation = std::make_unique<Animation>(Animation{
		.particles = StarParticles(
			StarParticles::Type::Right,
			15,
			st::lineWidth * 8),
	});
	_animation->particles.setSpeed(0.05);
}

void SummaryHeader::ensureLottie() const {
	_lottie = Lottie::MakeIcon(Lottie::IconDescriptor{
		.name = u"cocoon"_q,
		.color = &st::attentionButtonFg,
		.sizeOverride = Size(st::historySummaryHeaderIconSize
			- st::historySummaryHeaderIconSizeInner * 2),
		.colorizeUsingAlpha = true,
	});
}

} // namespace HistoryView
