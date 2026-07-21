/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_contact.h"

#include "boxes/add_contact_box.h"
#include "core/click_handler_types.h" // ClickHandlerContext
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item_components.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_reply.h"
#include "history/view/media/history_view_media_common.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/chat/chat_style.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/rect.h"
#include "ui/text/format_values.h" // Ui::FormatPhone
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h" // Ui::Text::Wrapped.
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"

namespace HistoryView {
namespace {

class ContactClickHandler final : public LambdaClickHandler {
public:
	using LambdaClickHandler::LambdaClickHandler;

	void setDragText(const QString &t) {
		_dragText = t;
	}

	QString dragText() const override final {
		return _dragText;
	}

private:
	QString _dragText;

};

ClickHandlerPtr SendMessageClickHandler(not_null<PeerData*> peer) {
	const auto clickHandlerPtr = std::make_shared<ContactClickHandler>([peer](
			ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		if (const auto controller = my.sessionWindow.get()) {
			if (controller->session().uniqueId()
					!= peer->session().uniqueId()) {
				return;
			}
			controller->showPeerHistory(
				peer->id,
				Window::SectionShow::Way::Forward);
		}
	});
	if (const auto user = peer->asUser()) {
		clickHandlerPtr->setDragText(user->phone().isEmpty()
			? peer->name()
			: Ui::FormatPhone(user->phone()));
	}
	return clickHandlerPtr;
}

ClickHandlerPtr AddContactClickHandler(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto sharedContact = [=, fullId = item->fullId()] {
		if (const auto item = session->data().message(fullId)) {
			if (const auto media = item->media()) {
				return media->sharedContact();
			}
		}
		return (const Data::SharedContact *)nullptr;
	};
	const auto clickHandlerPtr = std::make_shared<ContactClickHandler>([=](
			ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		if (const auto controller = my.sessionWindow.get()) {
			if (controller->session().uniqueId() != session->uniqueId()) {
				return;
			}
			if (const auto contact = sharedContact()) {
				controller->show(Box<AddContactBox>(
					session,
					contact->firstName,
					contact->lastName,
					contact->phoneNumber));
			}
		}
	});
	if (const auto contact = sharedContact()) {
		clickHandlerPtr->setDragText(Ui::FormatPhone(contact->phoneNumber));
	}
	return clickHandlerPtr;
}

[[nodiscard]] Fn<void(not_null<Ui::GenericBox*>)> VcardBoxFactory(
		const Data::SharedContact::VcardItems &vcardItems) {
	if (vcardItems.empty()) {
		return nullptr;
	}
	return [=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_contact_details_title());
		const auto &stL = st::proxyApplyBoxLabel;
		const auto &stSubL = st::boxDividerLabel;
		const auto add = [&](const QString &s, tr::phrase<> phrase) {
			if (!s.isEmpty()) {
				const auto label = box->addRow(
					object_ptr<Ui::FlatLabel>(box, s, stL));
				box->addRow(object_ptr<Ui::FlatLabel>(box, phrase(), stSubL));
				Ui::AddSkip(box->verticalLayout());
				Ui::AddSkip(box->verticalLayout());
				return label;
			}
			return (Ui::FlatLabel*)(nullptr);
		};
		for (const auto &[type, value] : vcardItems) {
			using Type = Data::SharedContact::VcardItemType;
			const auto isPhoneType = (type == Type::Phone)
				|| (type == Type::PhoneMain)
				|| (type == Type::PhoneHome)
				|| (type == Type::PhoneMobile)
				|| (type == Type::PhoneWork)
				|| (type == Type::PhoneOther);
			const auto typePhrase = (type == Type::Phone)
				? tr::lng_contact_details_phone
				: (type == Type::PhoneMain)
				? tr::lng_contact_details_phone_main
				: (type == Type::PhoneHome)
				? tr::lng_contact_details_phone_home
				: (type == Type::PhoneMobile)
				? tr::lng_contact_details_phone_mobile
				: (type == Type::PhoneWork)
				? tr::lng_contact_details_phone_work
				: (type == Type::PhoneOther)
				? tr::lng_contact_details_phone_other
				: (type == Type::Email)
				? tr::lng_contact_details_email
				: (type == Type::Address)
				? tr::lng_contact_details_address
				: (type == Type::Url)
				? tr::lng_contact_details_url
				: (type == Type::Note)
				? tr::lng_contact_details_note
				: (type == Type::Birthday)
				? tr::lng_contact_details_birthday
				: (type == Type::Organization)
				? tr::lng_contact_details_organization
				: tr::lng_payments_info_name;
			if (const auto label = add(value, typePhrase)) {
				const auto copyText = isPhoneType
					? tr::lng_profile_copy_phone
					: (type == Type::Email)
					? tr::lng_context_copy_email
					: (type == Type::Url)
					? tr::lng_context_copy_link
					: (type == Type::Name)
					? tr::lng_profile_copy_fullname
					: tr::lng_context_copy_text;
				label->setContextCopyText(copyText(tr::now));
				if (type == Type::Email) {
					label->setMarkedText(
						Ui::Text::Wrapped({ value }, EntityType::Email));
				} else if (type == Type::Url) {
					label->setMarkedText(
						Ui::Text::Wrapped({ value }, EntityType::Url));
				} else if (isPhoneType) {
					label->setText(Ui::FormatPhone(value));
				}
				using Request = Ui::FlatLabel::ContextMenuRequest;
				label->setContextMenuHook([=](Request r) {
					label->fillContextMenu(r.link
						? r
						: Request{ .menu = r.menu, .fullSelection = true });
				});
			}
		}
		{
			const auto inner = box->verticalLayout();
			if (inner->count() > 2) {
				delete inner->widgetAt(inner->count() - 1);
				delete inner->widgetAt(inner->count() - 1);
			}
		}

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	};
}

} // namespace

Contact::Contact(
	not_null<Element*> parent,
	const Data::SharedContact &data)
: Media(parent)
, _st(st::historyPagePreview)
, _pixh(st::contactsPhotoSize)
, _userId(data.userId)
, _vcardBoxFactory(VcardBoxFactory(data.vcardItems)) {
	history()->owner().registerContactView(data.userId, parent);

	_nameLine.setText(
		st::webPageTitleStyle,
		tr::lng_full_name(
			tr::now,
			lt_first_name,
			data.firstName,
			lt_last_name,
			data.lastName).trimmed(),
		Ui::WebpageTextTitleOptions());

	_phoneLine.setText(
		st::webPageDescriptionStyle,
		Ui::FormatPhone(data.phoneNumber),
		Ui::WebpageTextTitleOptions());
}

Contact::~Contact() {
	history()->owner().unregisterContactView(_userId, _parent);
	if (!_userpic.null()) {
		_userpic = {};
		_parent->checkHeavyPart();
	}
}

void Contact::updateSharedContactUserId(UserId userId) {
	if (_userId != userId) {
		history()->owner().unregisterContactView(_userId, _parent);
		_userId = userId;
		history()->owner().registerContactView(_userId, _parent);
	}
}

QSize Contact::countOptimalSize() {
	_contact = _userId
		? _parent->data()->history()->owner().userLoaded(_userId)
		: nullptr;
	if (_contact) {
		_contact->loadUserpic();
	} else {
		const auto full = _nameLine.toString();
		_photoEmpty = std::make_unique<Ui::EmptyUserpic>(
			Ui::EmptyUserpic::UserpicColor(Data::DecideColorIndex(_userId
				? peerFromUser(_userId)
				: Data::FakePeerIdForJustName(full))),
			full);
	}

	const auto vcardBoxFactory = _vcardBoxFactory;
	for (auto &repaint : _buttonRippleRepaints) {
		invalidateRippleRepaint(repaint);
		repaint.generation = ++_nextRippleGeneration;
	}
	_buttons.clear();
	if (_contact) {
		const auto message = tr::lng_contact_send_message(tr::now).toUpper();
		_buttons.push_back({
			message,
			st::semiboldFont->width(message),
			SendMessageClickHandler(_contact),
		});
		if (!_contact->isContact()) {
			const auto add = tr::lng_contact_add(tr::now).toUpper();
			_buttons.push_back({
				add,
				st::semiboldFont->width(add),
				AddContactClickHandler(_parent->data()),
			});
		}
		_mainButton.link = _buttons.front().link;
	} else if (vcardBoxFactory) {
		const auto view = tr::lng_contact_details_button(tr::now).toUpper();
		_buttons.push_back({
			view,
			st::semiboldFont->width(view),
			AddContactClickHandler(_parent->data()),
		});
	}
	if (vcardBoxFactory) {
		_mainButton.link = std::make_shared<LambdaClickHandler>([=](
				const ClickContext &context) {
			const auto my = context.other.value<ClickHandlerContext>();
			if (const auto controller = my.sessionWindow.get()) {
				controller->uiShow()->show(Box(vcardBoxFactory));
			}
		});
	}

	const auto padding = inBubblePadding() + innerMargin();
	const auto full = Rect(currentSize());
	const auto outer = full - inBubblePadding();
	const auto inner = outer - innerMargin();
	const auto lineLeft = inner.left() + _pixh + inner.left() - outer.left();
	const auto lineHeight = UnitedLineHeight();

	auto maxWidth = _parent->skipBlockWidth();
	auto minHeight = 0;

	auto textMinHeight = 0;
	if (!_nameLine.isEmpty()) {
		accumulate_max(maxWidth, lineLeft + _nameLine.maxWidth());
		textMinHeight += 1 * lineHeight;
	}
	if (!_phoneLine.isEmpty()) {
		accumulate_max(maxWidth, lineLeft + _phoneLine.maxWidth());
		textMinHeight += 1 * lineHeight;
	}
	minHeight = std::max(textMinHeight, st::contactsPhotoSize);

	if (!_buttons.empty()) {
		auto buttonsWidth = rect::m::sum::h(st::historyPageButtonPadding);
		for (const auto &button : _buttons) {
			buttonsWidth += button.width;
		}
		accumulate_max(maxWidth, buttonsWidth);
	}
	maxWidth += rect::m::sum::h(padding);
	minHeight += rect::m::sum::v(padding);

	return { maxWidth, minHeight };
}

void Contact::draw(Painter &p, const PaintContext &context) const {
	const auto canonical = context.hasElementPainter(p);
	if (canonical && _rippleLayoutSize != currentSize()) {
		_rippleLayoutSize = currentSize();
		invalidateRippleRepaints();
	}
	if (width() < rect::m::sum::h(st::msgPadding) + 1) {
		if (canonical) {
			recordRippleRepaint(
				_mainRippleRepaint,
				p,
				context,
				QRect());
			for (auto &repaint : _buttonRippleRepaints) {
				recordRippleRepaint(
					repaint,
					p,
					context,
					QRect());
			}
		}
		return;
	}

	const auto st = context.st;
	const auto stm = context.messageStyle();

	const auto full = Rect(currentSize());
	const auto outer = full - inBubblePadding();
	const auto inner = outer - innerMargin();
	auto tshift = inner.top();

	const auto selected = context.selected();
	const auto view = parent();
	const auto colorIndex = _contact
		? _contact->colorIndex()
		: Data::DecideColorIndex(
			Data::FakePeerIdForJustName(_nameLine.toString()));
	const auto &colorCollectible = _contact
		? _contact->colorCollectible()
		: nullptr;
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
	const auto backgroundEmojiId = _contact
		? _contact->backgroundEmojiId()
		: DocumentId();
	const auto backgroundEmojiData = backgroundEmojiId
		? st->backgroundEmojiData(backgroundEmojiId, colorCollectible).get()
		: nullptr;
	const auto backgroundEmojiCache = !backgroundEmojiData
		? nullptr
		: useColorCollectible
		? &backgroundEmojiData->collectibleCaches[colorCollectible]
		: &backgroundEmojiData->caches[Ui::BackgroundEmojiData::CacheIndex(
			selected,
			context.outbg,
			true,
			useColorIndex ? (colorIndex + 1) : 0)];
	Ui::Text::ValidateQuotePaintCache(*cache, _st);
	Ui::Text::FillQuotePaint(p, outer, *cache, _st);
	if (backgroundEmojiData) {
		ValidateBackgroundEmoji(
			backgroundEmojiId,
			colorCollectible,
			backgroundEmojiData,
			backgroundEmojiCache,
			cache,
			view);
		if (!backgroundEmojiCache->frames[0].isNull()) {
			const auto end = rect::bottom(inner) + _st.padding.bottom();
			const auto r = outer
				- QMargins(0, 0, 0, rect::bottom(outer) - end);
			FillBackgroundEmoji(
				p,
				r,
				false,
				*backgroundEmojiCache,
				backgroundEmojiData->firstGiftFrame);
		}
	}

	if (canonical
		&& _mainButton.ripple
		&& _mainButton.rippleSize != outer.size()) {
		_mainButton.ripple = nullptr;
		_mainButton.rippleSize = QSize();
		_mainRippleRepaint.generation = ++_nextRippleGeneration;
	}
	recordRippleRepaint(
		_mainRippleRepaint,
		p,
		context,
		outer);
	if (_mainButton.ripple) {
		p.save();
		p.translate(outer.topLeft());
		_mainButton.ripple->paint(
			p,
			0,
			0,
			_mainButton.rippleSize.width(),
			&cache->bg);
		p.restore();
		if (_mainButton.ripple->empty()) {
			_mainButton.ripple = nullptr;
			_mainButton.rippleSize = QSize();
			_mainRippleRepaint.generation = ++_nextRippleGeneration;
		}
	}

	{
		const auto left = inner.left();
		const auto top = tshift;
		if (_userId) {
			if (_contact) {
				const auto was = !_userpic.null();
				_contact->paintUserpic(p, _userpic, left, top, _pixh);
				if (!was && !_userpic.null()) {
					history()->owner().registerHeavyViewPart(_parent);
				}
			} else {
				_photoEmpty->paintCircle(p, left, top, _pixh, _pixh);
			}
		} else {
			_photoEmpty->paintCircle(p, left, top, _pixh, _pixh);
		}
		if (context.selected()) {
			auto hq = PainterHighQualityEnabler(p);
			p.setBrush(p.textPalette().selectOverlay);
			p.setPen(Qt::NoPen);
			p.drawEllipse(left, top, _pixh, _pixh);
		}
	}

	const auto lineHeight = UnitedLineHeight();
	const auto lineLeft = inner.left() + _pixh + inner.left() - outer.left();
	const auto lineWidth = rect::right(inner) - lineLeft;

	{
		p.setPen(cache->icon);
		p.setTextPalette(useColorCollectible
			? st->collectibleTextPalette(selected, colorCollectible)
			: useColorIndex
			? st->coloredTextPalette(selected, colorIndex)
			: stm->semiboldPalette);

		const auto endskip = _nameLine.hasSkipBlock()
			? _parent->skipBlockWidth()
			: 0;
		_nameLine.drawLeftElided(
			p,
			lineLeft,
			tshift,
			lineWidth,
			width(),
			1,
			style::al_left,
			0,
			-1,
			endskip,
			false,
			context.selection);
		tshift += lineHeight;

		p.setTextPalette(stm->textPalette);
	}
	p.setPen(stm->historyTextFg);
	{
		tshift += st::lineWidth * 3; // Additional skip.
		const auto endskip = _phoneLine.hasSkipBlock()
			? _parent->skipBlockWidth()
			: 0;
		_phoneLine.drawLeftElided(
			p,
			lineLeft,
			tshift,
			lineWidth,
			width(),
			1,
			style::al_left,
			0,
			-1,
			endskip,
			false,
			toTitleSelection(context.selection));
		tshift += 1 * lineHeight;
	}

	if (!_buttons.empty()) {
		p.setFont(st::semiboldFont);
		p.setPen(cache->icon);
		const auto end = rect::bottom(inner) + _st.padding.bottom();
		const auto line = st::historyPageButtonLine;
		auto color = cache->icon;
		color.setAlphaF(color.alphaF() * 0.3);
		const auto top = end + st::historyPageButtonPadding.top();
		const auto buttonWidth = inner.width() / float64(_buttons.size());
		p.fillRect(inner.x(), end, inner.width(), line, color);
		for (auto i = 0; i < _buttons.size(); i++) {
			const auto &button = _buttons[i];
			const auto left = inner.x() + i * buttonWidth;
			const auto rect = buttonRect(inner, outer, i);
			auto &repaint = buttonRippleRepaint(i);
			if (canonical
				&& button.ripple
				&& button.rippleSize != rect.size()) {
				button.ripple = nullptr;
				button.rippleSize = QSize();
				repaint.generation = ++_nextRippleGeneration;
			}
			recordRippleRepaint(repaint, p, context, rect);
			if (button.ripple) {
				p.save();
				p.translate(rect.topLeft());
				button.ripple->paint(
					p,
					0,
					0,
					button.rippleSize.width(),
					&cache->bg);
				p.restore();
				if (button.ripple->empty()) {
					_buttons[i].ripple = nullptr;
					_buttons[i].rippleSize = QSize();
					repaint.generation = ++_nextRippleGeneration;
				}
			}
			p.drawText(
				left + (buttonWidth - button.width) / 2,
				top + st::semiboldFont->ascent,
				button.text);
		}
	}
	for (auto i = _buttons.size(); i < _buttonRippleRepaints.size(); ++i) {
		recordRippleRepaint(
			_buttonRippleRepaints[i],
			p,
			context,
			QRect());
	}
	if (canonical && _buttonRippleRepaints.size() > _buttons.size()) {
		_buttonRippleRepaints.resize(_buttons.size());
	}
}

TextState Contact::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);

	const auto full = Rect(currentSize());
	const auto outer = full - inBubblePadding();
	const auto inner = outer - innerMargin();

	_lastPoint = point;

	if (!hasSingleLink()) {
		const auto end = rect::bottom(inner) + _st.padding.bottom();
		const auto bWidth = inner.width() / float64(_buttons.size());
		const auto bHeight = rect::bottom(outer) - end;
		for (auto i = 0; i < _buttons.size(); i++) {
			const auto left = inner.x() + i * bWidth;
			if (QRectF(left, end, bWidth, bHeight).contains(point)) {
				result.link = _buttons[i].link;
				return result;
			}
		}
	}
	if (outer.contains(point)) {
		result.link = _mainButton.link;
		return result;
	}
	return result;
}

void Contact::unloadHeavyPart() {
	_userpic = {};
}

bool Contact::hasHeavyPart() const {
	return !_userpic.null();
}

bool Contact::hasSingleLink() const {
	return (_buttons.size() > 1)
		? false
		: (_buttons.size() == 1 && _buttons.front().link == _mainButton.link)
		? true
		: (_buttons.empty() && _mainButton.link);
}

void Contact::clickHandlerPressedChanged(
		const ClickHandlerPtr &p,
		bool pressed) {
	const auto full = Rect(currentSize());
	const auto outer = full - inBubblePadding();
	const auto inner = outer - innerMargin();
	const auto end = rect::bottom(inner) + _st.padding.bottom();
	if ((_lastPoint.y() < end) || hasSingleLink()) {
		if (p != _mainButton.link) {
			return;
		}
		if (pressed) {
			if (!_mainButton.ripple) {
				const auto weak = base::make_weak(this);
				const auto generation = resetRippleRepaint(
					_mainRippleRepaint);
				_mainButton.rippleSize = outer.size();
				_mainButton.ripple = std::make_unique<Ui::RippleAnimation>(
					st::defaultRippleAnimation,
					Ui::RippleAnimation::RoundRectMask(
						outer.size(),
						_st.radius),
					[weak, generation] {
						if (const auto strong = weak.get()) {
							strong->repaintMainRipple(generation);
						}
					});
			}
			_mainButton.ripple->add(_lastPoint - outer.topLeft());
		} else if (_mainButton.ripple) {
			_mainButton.ripple->lastStop();
		}
		return;
	} else if (_buttons.empty()) {
		return;
	}
	for (auto i = 0; i < _buttons.size(); i++) {
		const auto &button = _buttons[i];
		if (p != button.link) {
			continue;
		}
		if (pressed) {
			if (!button.ripple) {
				const auto rect = buttonRect(inner, outer, i);
				auto &repaint = buttonRippleRepaint(i);
				const auto weak = base::make_weak(this);
				const auto generation = resetRippleRepaint(repaint);
				_buttons[i].rippleSize = rect.size();
				_buttons[i].ripple = std::make_unique<Ui::RippleAnimation>(
					st::defaultRippleAnimation,
					Ui::RippleAnimation::MaskByDrawer(
						rect.size(),
						false,
						[=](QPainter &p) {
							p.drawRect(Rect(rect.size()));
						}),
					[weak, index = i, generation] {
						if (const auto strong = weak.get()) {
							strong->repaintButtonRipple(
								index,
								generation);
						}
					});
			}
			button.ripple->add(
				_lastPoint - buttonRect(inner, outer, i).topLeft());
		} else if (button.ripple) {
			button.ripple->lastStop();
		}
	}
}

void Contact::repaintMainRipple(uint64 generation) const {
	repaintRipple(_mainRippleRepaint, generation);
}

void Contact::repaintButtonRipple(int index, uint64 generation) const {
	if (index < 0
		|| index >= _buttons.size()
		|| index >= _buttonRippleRepaints.size()) {
		return;
	}
	repaintRipple(_buttonRippleRepaints[index], generation);
}

void Contact::repaintRipple(
		RippleRepaint &repaint,
		uint64 generation) const {
	if (repaint.generation != generation
		|| repaint.pending
		|| (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = 1;
	if (!repaint.known) {
		this->repaint();
	} else {
		repaintRippleRegion(repaint.current);
	}
}

void Contact::recordRippleRepaint(
		RippleRepaint &repaint,
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
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.pending = 0;
	repaint.current = known ? std::move(current) : QRegion();
	repaint.known = known ? 1 : 0;
	if (!known) {
		repaint.stale = previous;
		if (!previous.isEmpty()) {
			repaint.pending = 1;
			this->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = 1;
	repaintRippleRegion(previous.united(repaint.current));
}

void Contact::invalidateRippleRepaint(RippleRepaint &repaint) const {
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = 0;
	repaint.known = 0;
}

void Contact::invalidateRippleRepaints() const {
	invalidateRippleRepaint(_mainRippleRepaint);
	for (auto &repaint : _buttonRippleRepaints) {
		invalidateRippleRepaint(repaint);
	}
}

void Contact::repaintRippleRegion(const QRegion &region) const {
	_parent->repaint(region);
}

Contact::RippleRepaint &Contact::buttonRippleRepaint(int index) const {
	Expects(index >= 0);
	if (_buttonRippleRepaints.size() <= index) {
		_buttonRippleRepaints.resize(index + 1);
	}
	return _buttonRippleRepaints[index];
}

uint64 Contact::resetRippleRepaint(RippleRepaint &repaint) const {
	repaint.pending = 0;
	repaint.generation = ++_nextRippleGeneration;
	return repaint.generation;
}

QMargins Contact::inBubblePadding() const {
	return {
		st::msgPadding.left(),
		isBubbleTop() ? st::msgPadding.left() : 0,
		st::msgPadding.right(),
		isBubbleBottom() ? (st::msgPadding.left() + bottomInfoPadding()) : 0
	};
}

QMargins Contact::innerMargin() const {
	const auto button = _buttons.empty() ? 0 : st::historyPageButtonHeight;
	return _st.padding + QMargins(0, 0, 0, button);
}

int Contact::bottomInfoPadding() const {
	if (!isBubbleBottom()) {
		return 0;
	}

	auto result = st::msgDateFont->height;

	// We use padding greater than st::msgPadding.bottom() in the
	// bottom of the bubble so that the left line looks pretty.
	// but if we have bottom skip because of the info display
	// we don't need that additional padding so we replace it
	// back with st::msgPadding.bottom() instead of left().
	result += st::msgPadding.bottom() - st::msgPadding.left();
	return result;
}

QRect Contact::buttonRect(
		const QRect &inner,
		const QRect &outer,
		int index) const {
	Expects(index >= 0 && index < _buttons.size());
	const auto top = rect::bottom(inner) + _st.padding.bottom();
	const auto width = inner.width() / float64(_buttons.size());
	return {
		int(inner.x() + index * width),
		top,
		int(width),
		rect::bottom(outer) - top,
	};
}

TextSelection Contact::toTitleSelection(TextSelection selection) const {
	return UnshiftItemSelection(selection, _nameLine);
}

TextSelection Contact::toDescriptionSelection(TextSelection selection) const {
	return UnshiftItemSelection(toTitleSelection(selection), _phoneLine);
}

} // namespace HistoryView
