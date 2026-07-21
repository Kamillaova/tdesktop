/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_poll.h"

#include "core/click_handler_types.h"
#include "core/ui_integration.h" // TextContext
#include "data/data_cloud_file.h"
#include "data/data_location.h"
#include "lang/lang_keys.h"
#include "lang/lang_tag.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_message.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_reaction_preview.h"
#include "history/view/history_view_text_helper.h"
#include "history/view/media/menu/history_view_poll_menu.h"
#include "calls/calls_instance.h"
#include "ui/widgets/dropdown_menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/chat/message_bubble.h"
#include "ui/chat/chat_style.h"
#include "ui/image/image.h"
#include "ui/item_text_options.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/text/format_values.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/fireworks_animation.h"
#include "ui/toast/toast.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/dynamic_image.h"
#include "ui/dynamic_thumbnails.h"
#include "poll/poll_link_thumbnail.h"
#include "poll/poll_media_upload.h"
#include "history/view/media/history_view_location.h"
#include "history/view/media/history_view_media_common.h"
#include "history/view/history_view_group_call_bar.h"
#include "data/data_media_types.h"
#include "data/data_document.h"
#include "data/data_channel.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_file_origin.h"
#include "data/data_poll.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "data/stickers/data_custom_emoji.h"
#include "base/crc32hash.h"
#include "base/unixtime.h"
#include "base/timer.h"
#include "base/qt/qt_key_modifiers.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "api/api_polls.h"
#include "window/window_session_controller.h"
#include "ui/layers/generic_box.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/labels.h"
#include "history/view/controls/history_view_webpage_processor.h"
#include "history/view/media/history_view_web_page.h"
#include "history/admin_log/history_admin_log_item.h"
#include "history/history.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/effects/path_shift_gradient.h"
#include "ui/paint/damage.h"
#include "window/themes/window_theme.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"
#include "styles/style_polls.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"


namespace HistoryView {
namespace {

constexpr auto kShowRecentVotersCount = 3;
constexpr auto kRotateSegments = 8;
constexpr auto kRotateAmplitude = 3.;
constexpr auto kScaleSegments = 2;
constexpr auto kScaleAmplitude = 0.03;
constexpr auto kRollDuration = crl::time(400);
constexpr auto kExpiringVoteRestrictionDuration = 10 * 60 * crl::time(1000);
constexpr auto kVoteRestrictionToastDuration = 5 * crl::time(1000);

[[nodiscard]] QMargins PollBubbleRollRepaintMargins(QSize size) {
	const auto radians = kRotateAmplitude * M_PI / 180.;
	const auto sine = std::sin(radians);
	const auto cosine = std::cos(radians);
	const auto scale = 1. + kScaleAmplitude;
	const auto addX = int(std::ceil(std::max(
		0.,
		(scale * (size.width() * cosine + size.height() * sine)
			- size.width()) / 2.))) + st::lineWidth;
	const auto addY = int(std::ceil(std::max(
		0.,
		(scale * (size.height() * cosine + size.width() * sine)
			- size.height()) / 2.))) + st::lineWidth;
	return QMargins(addX, addY, addX, addY);
}

[[nodiscard]] bool IsExpiringVoteRestriction(
		PollData::VoteRestriction restriction) {
	using Restriction = PollData::VoteRestriction;
	return (restriction == Restriction::SubscribersOnly)
		|| (restriction == Restriction::SubscribersJoinedTooRecently);
}

[[nodiscard]] int PollAnswerMediaSize() {
	return st::historyPollRadio.diameter * 2;
}

[[nodiscard]] int PollAnswerMediaSkip() {
	return st::historyPollPercentSkip * 2;
}

[[nodiscard]] bool HasPollAnswerTextAnimation(
		const Ui::Text::String &text) {
	return text.hasCustomEmoji() || text.hasSpoilers();
}

[[nodiscard]] bool AddPollTextRepaintRect(
		QRegion &region,
		const Painter &p,
		const PaintContext &context,
		const Ui::Text::String &text,
		QRect rect,
		const Ui::Text::CustomEmojiRepaintBounds
			&customEmojiRepaintBounds,
		bool useFullWidth = false) {
	if (!HasPollAnswerTextAnimation(text)) {
		return true;
	}
	const auto repaintRect = customEmojiRepaintBounds.rect;
	if (!repaintRect.isEmpty()) {
		const auto mapped = context.mapToElement(p, repaintRect);
		if (!mapped) {
			return false;
		} else if (!mapped->isEmpty()) {
			region += *mapped;
		}
	}
	if (customEmojiRepaintBounds.repaintBoundsKnown
		&& !text.hasSpoilers()) {
		return true;
	}
	const auto layoutWidth = useFullWidth
		? rect.width()
		: std::min(rect.width(), text.maxWidth());
	const auto lines = text.countLinesGeometry(layoutWidth);
	if (lines.empty()) {
		return false;
	}
	auto mappedRegion = QRegion();
	auto lineTop = 0;
	for (const auto &line : lines) {
		const auto lineBottom = std::clamp(
			line.bottom,
			lineTop,
			rect.height());
		auto lineLeft = std::clamp(line.left, 0, layoutWidth);
		const auto lineWidth = std::clamp(
			line.width,
			0,
			layoutWidth - lineLeft);
		if (line.rtl) {
			lineLeft = layoutWidth - lineLeft - lineWidth;
		}
		if (lineWidth > 0 && lineBottom > lineTop) {
			const auto lineRect = QRectF(
				rect.x() + lineLeft,
				rect.y() + lineTop,
				lineWidth,
				lineBottom - lineTop);
			const auto mapped = context.mapToElement(p, lineRect);
			if (!mapped) {
				return false;
			} else if (!mapped->isEmpty()) {
				mappedRegion += *mapped;
			}
		}
		lineTop = lineBottom;
	}
	region += mappedRegion;
	return true;
}

enum class PollThumbnailKind {
	None,
	Photo,
	Document,
	Audio,
	Emoji,
	Geo,
	Webpage,
};

struct PercentCounterItem {
	int index = 0;
	int percent = 0;
	int remainder = 0;

	inline bool operator==(const PercentCounterItem &o) const {
		return remainder == o.remainder && percent == o.percent;
	}

	inline bool operator<(const PercentCounterItem &other) const {
		if (remainder > other.remainder) {
			return true;
		} else if (remainder < other.remainder) {
			return false;
		}
		return percent < other.percent;
	}
};

void AdjustPercentCount(gsl::span<PercentCounterItem> items, int left) {
	ranges::sort(items, std::less<>());
	for (auto i = 0, count = int(items.size()); i != count;) {
		const auto &item = items[i];
		auto j = i + 1;
		for (; j != count; ++j) {
			if (items[j].percent != item.percent
				|| items[j].remainder != item.remainder) {
				break;
			}
		}
		if (!items[i].remainder) {
			// If this item has correct value in 'percent' we don't want
			// to increment it to an incorrect one. This fixes a case with
			// four items with three votes for three different items.
			break;
		}
		const auto equal = j - i;
		if (equal <= left) {
			left -= equal;
			for (; i != j; ++i) {
				++items[i].percent;
			}
		} else {
			i = j;
		}
	}
}

void CountNicePercent(
		gsl::span<const int> votes,
		int total,
		gsl::span<int> result) {
	Expects(result.size() >= votes.size());
	Expects(votes.size() <= PollData::kMaxOptions);

	const auto count = size_type(votes.size());
	PercentCounterItem ItemsStorage[PollData::kMaxOptions];
	const auto items = gsl::make_span(ItemsStorage).subspan(0, count);
	auto left = 100;
	auto &&zipped = ranges::views::zip(
		votes,
		items,
		ranges::views::ints(0, int(items.size())));
	for (auto &&[votes, item, index] : zipped) {
		item.index = index;
		item.percent = (votes * 100) / total;
		item.remainder = (votes * 100) - (item.percent * total);
		left -= item.percent;
	}
	if (left > 0 && left <= count) {
		AdjustPercentCount(items, left);
	}
	for (const auto &item : items) {
		result[item.index] = item.percent;
	}
}

[[nodiscard]] uint32 HashPollShuffleValue(
		UserId userId,
		PollId pollId,
		const QByteArray &option) {
	auto hash = QByteArray::number(quint64(userId.bare))
		+ option
		+ QByteArray::number(quint64(pollId));
	return uint32(base::crc32(hash.constData(), hash.size()));
}

[[nodiscard]] bool WebPageHasRichPreview(WebPageData *webpage) {
	return webpage
		&& (!webpage->title.isEmpty()
			|| !webpage->description.text.isEmpty()
			|| webpage->photo
			|| !webpage->siteName.isEmpty());
}

class OpenLinkPreviewDelegate final
	: public HistoryView::SimpleElementDelegate {
public:
	using HistoryView::SimpleElementDelegate::SimpleElementDelegate;

	HistoryView::Context elementContext() override {
		return HistoryView::Context::History;
	}
};

class OpenLinkPreviewWidget final : public Ui::RpWidget {
public:
	OpenLinkPreviewWidget(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<WebPageData*> webpage,
		Fn<void()> close);
	~OpenLinkPreviewWidget();

private:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void resizeMedia(int width);
	void updateActiveLink(QPoint point);

	const base::weak_ptr<Window::SessionController> _controller;
	const Fn<void()> _close;
	const std::unique_ptr<Ui::ChatTheme> _theme;
	const std::unique_ptr<Ui::ChatStyle> _style;
	const std::unique_ptr<OpenLinkPreviewDelegate> _delegate;
	const not_null<History*> _history;
	AdminLog::OwnedItem _item;

};

OpenLinkPreviewWidget::OpenLinkPreviewWidget(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller,
	not_null<WebPageData*> webpage,
	Fn<void()> close)
: RpWidget(parent)
, _controller(base::make_weak(controller))
, _close(std::move(close))
, _theme(Window::Theme::DefaultChatThemeOn(lifetime()))
, _style(std::make_unique<Ui::ChatStyle>(
	controller->session().colorIndicesValue()))
, _delegate(std::make_unique<OpenLinkPreviewDelegate>(
	controller,
	[=] { update(); }))
, _history(controller->session().data().history(
	controller->session().userPeerId())) {
	_style->apply(_theme.get());
	setMouseTracking(true);

	const auto item = _history->makeMessage({
		.id = _history->nextNonHistoryEntryId(),
		.flags = (MessageFlag::FakeHistoryItem | MessageFlag::Local),
		.from = _history->peer->id,
	}, TextWithEntities(), MTP_messageMediaEmpty());
	auto owned = AdminLog::OwnedItem(_delegate.get(), item);
	owned->overrideMedia(std::make_unique<HistoryView::WebPage>(
		owned.get(),
		webpage,
		MediaWebPageFlags{}));
	_item = std::move(owned);
	if (const auto media = _item->media()) {
		media->setInBubbleState(MediaInBubbleState::Middle);
		media->initDimensions();
	}

	_history->session().downloaderTaskFinished(
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	_history->owner().viewRepaintRequest(
	) | rpl::on_next([=](Data::RequestViewRepaint data) {
		if (data.view == _item.get()) {
			update();
		}
	}, lifetime());

	widthValue(
	) | rpl::filter([=](int w) {
		return w > 0;
	}) | rpl::on_next([=](int w) {
		resizeMedia(w);
	}, lifetime());
}

OpenLinkPreviewWidget::~OpenLinkPreviewWidget() {
	const auto raw = _item.get();
	if (Element::Pressed() == raw) {
		Element::Pressed(nullptr);
	}
	if (Element::Hovered() == raw) {
		Element::Hovered(nullptr);
	}
	if (Element::PressedLink() == raw) {
		Element::PressedLink(nullptr);
	}
	if (Element::HoveredLink() == raw) {
		Element::HoveredLink(nullptr);
	}
	if (Element::Moused() == raw) {
		Element::Moused(nullptr);
	}
}

void OpenLinkPreviewWidget::resizeMedia(int width) {
	if (const auto media = _item->media()) {
		const auto height = media->resizeGetHeight(width);
		resize(width, height);
	}
}

void OpenLinkPreviewWidget::paintEvent(QPaintEvent *e) {
	const auto media = _item->media();
	if (!media) {
		return;
	}
	auto p = Painter(this);
	auto context = _theme->preparePaintContext(
		_style.get(),
		rect(),
		rect(),
		e->rect(),
		!window()->isActiveWindow());
	media->draw(p, context);
}

void OpenLinkPreviewWidget::updateActiveLink(QPoint point) {
	const auto media = _item->media();
	if (!media) {
		return;
	}
	const auto state = media->textState(point, StateRequest());
	ClickHandler::setActive(state.link, _item.get());
	setCursor(state.link ? style::cur_pointer : style::cur_default);
}

void OpenLinkPreviewWidget::mouseMoveEvent(QMouseEvent *e) {
	updateActiveLink(e->pos());
}

void OpenLinkPreviewWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	updateActiveLink(e->pos());
	Element::Pressed(_item.get());
	ClickHandler::pressed();
}

void OpenLinkPreviewWidget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto activated = ClickHandler::unpressed();
	if (Element::Pressed() == _item.get()) {
		Element::Pressed(nullptr);
	}
	if (!activated) {
		return;
	}
	const auto externalUrl = activated->url();
	if (!externalUrl.isEmpty()) {
		UrlClickHandler::Open(externalUrl);
	} else if (const auto controller = _controller.get()) {
		activated->onClick({
			e->button(),
			QVariant::fromValue(ClickHandlerContext{
				.itemId = _item->data()->fullId(),
				.sessionWindow = _controller,
				.show = controller->uiShow(),
			})
		});
	}
	if (_close) {
		_close();
	}
}

void OpenLinkPreviewWidget::leaveEventHook(QEvent *e) {
	ClickHandler::clearActive(_item.get());
	setCursor(style::cur_default);
	RpWidget::leaveEventHook(e);
}

void OpenPollOptionLinkBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QString url,
		WebPageData *webpage) {
	box->setTitle(tr::lng_polls_option_open_link_title());
	const auto content = box->verticalLayout();

	const auto openAndClose = [=] {
		UrlClickHandler::Open(url);
		box->closeBox();
	};

	const auto urlContainer = content->add(
		object_ptr<Ui::RpWidget>(content),
		st::pollOpenLinkUrlOuterPadding);
	const auto urlLabel = Ui::CreateChild<Ui::FlatLabel>(
		urlContainer,
		rpl::single(url),
		st::pollOpenLinkUrlLabel);
	urlLabel->setBreakEverywhere(true);
	urlLabel->setSelectable(true);
	const auto inner = st::pollOpenLinkUrlInnerPadding;
	urlContainer->widthValue(
	) | rpl::on_next([=](int width) {
		urlLabel->resizeToWidth(
			std::max(width - inner.left() - inner.right(), 1));
		urlLabel->moveToLeft(inner.left(), inner.top());
	}, urlContainer->lifetime());
	urlLabel->heightValue(
	) | rpl::on_next([=](int height) {
		urlContainer->resize(
			urlContainer->width(),
			height + inner.top() + inner.bottom());
	}, urlContainer->lifetime());
	urlContainer->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(urlContainer);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(
			urlContainer->rect(),
			st::pollOpenLinkUrlRadius,
			st::pollOpenLinkUrlRadius);
	}, urlContainer->lifetime());

	if (webpage && !webpage->failed && WebPageHasRichPreview(webpage)) {
		content->add(
			object_ptr<OpenLinkPreviewWidget>(
				content,
				controller,
				webpage,
				[=] { box->closeBox(); }),
			st::pollOpenLinkPreviewOuterPadding);
	}

	box->addButton(tr::lng_open_link(), openAndClose);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

struct PollThumbnailData {
	std::shared_ptr<Ui::DynamicImage> thumbnail;
	ClickHandlerPtr handler;
	PollThumbnailKind kind = PollThumbnailKind::None;
	bool rounded = false;
	bool isVideo = false;
	uint64 id = 0;
};

[[nodiscard]] PollThumbnailData MakePollThumbnail(
		not_null<PollData*> poll,
		const PollAnswer &answer,
		Window::SessionController::MessageContext messageContext,
		Fn<bool()> paused = nullptr);

[[nodiscard]] PollThumbnailData MakePollThumbnail(
		not_null<PollData*> poll,
		const PollMedia &media,
		Window::SessionController::MessageContext messageContext,
		Fn<bool()> paused = nullptr) {
	auto result = PollThumbnailData();
	if (!media) {
		return result;
	}
	if (media.photo) {
		result.id = uint64(media.photo->id);
		result.thumbnail = Ui::MakePhotoThumbnailCenterCrop(
			media.photo,
			messageContext.id);
		result.rounded = true;
		result.kind = PollThumbnailKind::Photo;
	} else if (media.document) {
		result.id = uint64(media.document->id);
		if (media.document->sticker()) {
			result.thumbnail = Ui::MakeEmojiThumbnail(
				&poll->owner(),
				Data::SerializeCustomEmojiId(media.document),
				paused);
			result.kind = PollThumbnailKind::Emoji;
		} else if (media.document->isSong()
			|| media.document->isVoiceMessage()) {
			result.thumbnail = Ui::MakeDocumentFilePreviewThumbnail(
				media.document,
				messageContext.id);
			result.kind = PollThumbnailKind::Audio;
		} else {
			result.thumbnail = Ui::MakeDocumentThumbnailCenterCrop(
				media.document,
				messageContext.id);
			result.rounded = true;
			result.kind = PollThumbnailKind::Document;
			result.isVideo = media.document->isVideoFile()
				|| media.document->isVideoMessage()
				|| media.document->isAnimation();
		}
	} else if (media.geo) {
		result.id = uint64(media.geo->hash());
		const auto cloudImage = poll->owner().location(*media.geo);
		result.thumbnail = Ui::MakeGeoThumbnailWithPin(
			cloudImage,
			&poll->session(),
			Data::FileOrigin());
		result.rounded = true;
		result.kind = PollThumbnailKind::Geo;
	} else if (media.webpage || !media.url.isEmpty()) {
		const auto webpage = media.webpage;
		const auto photo = webpage ? webpage->photo : nullptr;
		if (photo) {
			result.id = uint64(photo->id);
			result.thumbnail = Ui::MakePhotoThumbnailCenterCrop(
				photo,
				messageContext.id);
			result.rounded = true;
		} else {
			result.thumbnail = ::Poll::MakeLinkThumbnail();
			result.rounded = true;
		}
		result.kind = PollThumbnailKind::Webpage;
	}
	if (result.kind == PollThumbnailKind::Photo && result.id) {
		const auto photo = media.photo;
		const auto session = &poll->session();
		result.handler = std::make_shared<LambdaClickHandler>(
			[=](ClickContext context) {
				const auto my = context.other.value<ClickHandlerContext>();
				const auto controller = my.sessionWindow.get();
				if (!controller || (&controller->session() != session)) {
					return;
				}
				controller->openPhoto(photo, messageContext);
			});
	} else if ((result.kind == PollThumbnailKind::Document
		|| result.kind == PollThumbnailKind::Audio) && result.id) {
		const auto document = media.document;
		const auto session = &poll->session();
		result.handler = std::make_shared<LambdaClickHandler>(
			[=](ClickContext context) {
				const auto my = context.other.value<ClickHandlerContext>();
				const auto controller = my.sessionWindow.get();
				if (!controller || (&controller->session() != session)) {
					return;
				}
				controller->openDocument(document, true, messageContext);
			});
	} else if (result.kind == PollThumbnailKind::Geo && media.geo) {
		const auto point = *media.geo;
		const auto session = &poll->session();
		result.handler = std::make_shared<LambdaClickHandler>(
			[=](ClickContext context) {
				const auto my = context.other.value<ClickHandlerContext>();
				const auto controller = my.sessionWindow.get();
				if (!controller || (&controller->session() != session)) {
					return;
				}
				HistoryView::ShowPollGeoPreview(controller, point);
			});
	} else if (result.kind == PollThumbnailKind::Webpage) {
		const auto url = !media.url.isEmpty()
			? media.url
			: (media.webpage ? media.webpage->url : QString());
		const auto webpage = media.webpage;
		const auto session = &poll->session();
		if (!url.isEmpty()) {
			result.handler = std::make_shared<LambdaClickHandler>(
				[=](ClickContext clickContext) {
					const auto my = clickContext.other.value<
						ClickHandlerContext>();
					const auto controller = my.sessionWindow.get();
					if (!controller
						|| (&controller->session() != session)) {
						UrlClickHandler::Open(url);
						return;
					}
					controller->show(Box(
						OpenPollOptionLinkBox,
						controller,
						url,
						webpage));
				});
		}
	}
	return result;
}

PollThumbnailData MakePollThumbnail(
		not_null<PollData*> poll,
		const PollAnswer &answer,
		Window::SessionController::MessageContext messageContext,
		Fn<bool()> paused) {
	auto result
		= MakePollThumbnail(poll, answer.media, messageContext, paused);
	if (result.kind == PollThumbnailKind::Emoji && result.id) {
		const auto documentId = DocumentId(result.id);
		const auto option = answer.option;
		const auto session = &poll->session();
		result.handler = std::make_shared<LambdaClickHandler>(
			[=](ClickContext context) {
				const auto my = context.other.value<ClickHandlerContext>();
				const auto controller = my.sessionWindow.get();
				if (!controller || (&controller->session() != session)) {
					return;
				}
				const auto document = poll->owner().document(documentId);
				const auto itemId = messageContext.id;
				ShowStickerPreview(
					controller,
					itemId,
					document,
					[=](not_null<Ui::DropdownMenu*> menu) {
						FillPollAnswerMenu(
							menu,
							poll,
							option,
							document,
							itemId,
							controller);
					});
			});
	}
	if (result.handler) {
		result.handler->setProperty(
			kPollOptionProperty,
			answer.option);
	}
	return result;
}

} // namespace

struct Poll::AnswerAnimation {
	anim::value percent;
	anim::value filling;
	anim::value opacity;
	bool chosen = false;
	bool correct = false;
};

struct Poll::AnswersAnimation {
	std::vector<AnswerAnimation> data;
	Ui::Animations::Simple progress;
	QRegion repaintRegion;
	QRegion collectedRepaintRegion;
	bool repaintPending = false;
	bool collectingRepaintRegion = false;
};

struct Poll::SendingAnimation {
	template <typename Callback>
	SendingAnimation(
		const QByteArray &option,
		Callback &&callback);

	QByteArray option;
	Ui::InfiniteRadialAnimation animation;
	QRegion repaintRegion;
	QRegion collectedRepaintRegion;
	bool repaintPending = false;
	bool collectingRepaintRegion = false;
};

struct Poll::Answer {
	Answer();

	void fillMedia(
		not_null<PollData*> poll,
		const PollAnswer &original,
		Window::SessionController::MessageContext messageContext,
		not_null<Options*> options,
		Fn<bool()> paused);

	Ui::Text::String text;
	QByteArray option;
	int votes = 0;
	int votesPercent = 0;
	int votesPercentWidth = 0;
	float64 filling = 0.;
	QString votesPercentString;
	QString votesCountString;
	int votesCountWidth = 0;
	bool chosen = false;
	bool correct = false;
	bool selected = false;
	ClickHandlerPtr handler;
	ClickHandlerPtr mediaHandler;
	Ui::Animations::Simple selectedAnimation;
	std::shared_ptr<Ui::DynamicImage> thumbnail;
	bool thumbnailRounded = false;
	bool thumbnailIsVideo = false;
	PollThumbnailKind thumbnailKind = PollThumbnailKind::None;
	uint64 thumbnailId = 0;
	uint64 textGeneration = 0;
	mutable std::unique_ptr<Ui::RippleAnimation> ripple;
	mutable QSize rippleMaskSize;
	std::vector<UserpicInRow> recentVoters;
	mutable QImage recentVotersImage;
};

struct Poll::AttachedMedia {
	std::shared_ptr<Ui::DynamicImage> thumbnail;
	ClickHandlerPtr handler;
	PhotoData *photo = nullptr;
	std::shared_ptr<Data::PhotoMedia> photoMedia;
	QSize photoSize;
	PollThumbnailKind kind = PollThumbnailKind::None;
	bool rounded = false;
	uint64 id = 0;
};

struct Poll::SolutionMedia {
	PollThumbnailKind kind = PollThumbnailKind::None;
	uint64 id = 0;
};

struct Poll::RecentVoter {
	not_null<PeerData*> peer;
	mutable Ui::PeerUserpicView userpic;
};

struct Poll::Part {
	explicit Part(not_null<Poll*> owner) : _owner(owner) {}
	virtual ~Part() = default;

	[[nodiscard]] virtual int countHeight(int innerWidth) const = 0;
	virtual void draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const = 0;
	[[nodiscard]] virtual TextState textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const = 0;
	virtual void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {}
	virtual void clickHandlerActiveChanged(
		const ClickHandlerPtr &handler,
		bool active) {}
	[[nodiscard]] virtual bool hasHeavyPart() const { return false; }
	virtual void unloadHeavyPart() {}
	[[nodiscard]] virtual uint16 selectionLength() const { return 0; }

protected:
	const not_null<Poll*> _owner;
};

struct Poll::Footer : public Poll::Part {
	explicit Footer(not_null<Poll*> owner);

	int countHeight(int innerWidth) const override;
	void draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const override;
	TextState textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const override;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;

	void updateTotalVotes();

	Ui::Text::String _totalVotesLabel;
	Ui::Text::String _adminVotesLabel;
	Ui::Text::String _adminBackVoteLabel;
	ClickHandlerPtr _showResultsLink;
	ClickHandlerPtr _sendVotesLink;
	ClickHandlerPtr _adminVotesLink;
	ClickHandlerPtr _adminBackVoteLink;
	ClickHandlerPtr _saveOptionLink;
	mutable std::unique_ptr<Ui::RippleAnimation> _linkRipple;
	mutable QImage _linkRippleMask;
	mutable QSize _linkRippleLayoutSize;
	mutable QSize _linkRippleMaskSize;
	mutable Ui::BubbleRounding _linkRippleMaskRounding;
	mutable int _linkRippleShift = 0;
	mutable RepaintState _contentRepaint;
	mutable RepaintState _rippleRepaint;
	mutable base::Timer _closeTimer;

private:
	struct Layout {
		enum class Kind {
			PassiveLabel,
			SaveOption,
			AdminVotes,
			AdminBack,
			LinkButton,
		};
		Kind kind = Kind::PassiveLabel;
		ClickHandlerPtr link;
		bool compact = false;
		bool timerFolded = false;
		bool timerSeparate = false;
		int topSkip = 0;
		int textY = 0;
		int timerY = 0;
		int totalHeight = 0;
	};
	[[nodiscard]] Layout computeLayout(int innerWidth) const;
	[[nodiscard]] bool hasCloseDate() const;
	[[nodiscard]] QString closeTimerText() const;
	[[nodiscard]] QRect timerRect(
		const Layout &layout,
		int left,
		int innerWidth) const;
	[[nodiscard]] bool timerFooterMultiline(int paintw) const;
	[[nodiscard]] bool centeredOverlapsInfo(
		int textWidth,
		int innerWidth) const;
	[[nodiscard]] QString linkButtonText() const;
	void prepareLinkRippleMask(const Layout &layout, int outerWidth) const;
	[[nodiscard]] QRect linkRippleRect(
		const Layout &layout,
		int outerWidth) const;
	void resetLinkRipple() const;
	void toggleLinkRipple(bool pressed);
};

bool Poll::Footer::hasCloseDate() const {
	return _owner->_poll->closeDate > 0
		&& !(_owner->_flags & PollData::Flag::Closed);
}

auto Poll::Footer::computeLayout(int innerWidth) const -> Layout {
	Layout result;
	result.compact = _owner->inlineFooter();
	result.topSkip = _owner->canAddOption()
		? 0
		: st::historyPollTotalVotesSkip;

	const auto timerText = closeTimerText();
	const auto hasTimer = !timerText.isEmpty();
	const auto lineHeight = st::msgDateFont->height;

	if (result.compact) {
		result.kind = Layout::Kind::PassiveLabel;
		if (hasTimer) {
			if (timerFooterMultiline(innerWidth)) {
				result.timerSeparate = true;
			} else {
				result.timerFolded = true;
			}
		}
	} else if (_owner->_addOptionActive) {
		result.kind = Layout::Kind::SaveOption;
		result.link = _saveOptionLink;
	} else if (_owner->isAuthorNotVoted()
		&& !_owner->_adminShowResults
		&& !_owner->canSendVotes()) {
		if (_owner->_totalVotes > 0) {
			result.kind = Layout::Kind::AdminVotes;
			result.link = _adminVotesLink;
		} else {
			result.kind = Layout::Kind::PassiveLabel;
		}
	} else if (_owner->_adminShowResults
		&& _owner->isAuthorNotVoted()) {
		result.kind = Layout::Kind::AdminBack;
		result.link = _adminBackVoteLink;
	} else if (_owner->showVotersCount()) {
		result.kind = Layout::Kind::PassiveLabel;
		if (hasTimer) {
			if (timerFooterMultiline(innerWidth)) {
				result.timerSeparate = true;
			} else {
				result.timerFolded = true;
			}
		}
	} else {
		result.kind = Layout::Kind::LinkButton;
		const auto votedPublic = _owner->_voted
			&& (_owner->_flags & PollData::Flag::PublicVotes);
		result.link = (_owner->showVotes() || votedPublic)
			? _showResultsLink
			: _owner->canSendVotes()
			? _sendVotesLink
			: nullptr;
		if (hasTimer) {
			result.timerSeparate = true;
		}
	}

	const auto buttonSkip = result.compact
		? 0
		: st::historyPollBottomButtonSkip;
	result.textY = result.compact
		? st::msgPadding.bottom()
		: (result.topSkip
			+ st::msgPadding.bottom()
			+ st::historyPollBottomButtonTop);
	result.timerY = result.textY + lineHeight;

	auto bottomW = 0;
	if (result.timerSeparate) {
		bottomW = st::msgDateFont->width(timerText);
	} else if (!result.timerFolded) {
		switch (result.kind) {
		case Layout::Kind::PassiveLabel:
			bottomW = _totalVotesLabel.maxWidth();
			break;
		case Layout::Kind::SaveOption:
			bottomW = st::semiboldFont->width(
				tr::lng_polls_add_option_save(tr::now));
			break;
		case Layout::Kind::AdminVotes:
			bottomW = _adminVotesLabel.maxWidth();
			break;
		case Layout::Kind::AdminBack:
			bottomW = _adminBackVoteLabel.maxWidth();
			break;
		case Layout::Kind::LinkButton:
			bottomW = st::semiboldFont->width(linkButtonText());
			break;
		}
	}
	const auto dateInfoPad = (bottomW > 0
		&& centeredOverlapsInfo(bottomW, innerWidth))
		? lineHeight
		: 0;

	result.totalHeight = result.topSkip
		+ buttonSkip
		+ lineHeight
		+ (result.timerSeparate ? lineHeight : 0)
		+ dateInfoPad
		+ st::msgPadding.bottom();

	return result;
}

QString Poll::Footer::linkButtonText() const {
	const auto votedPublic = _owner->_voted
		&& (_owner->_flags & PollData::Flag::PublicVotes);
	return (_owner->showVotes() || votedPublic)
		? ((_owner->_flags & PollData::Flag::PublicVotes)
			? tr::lng_polls_view_votes(
				tr::now,
				lt_count,
				_owner->_totalVotes)
			: tr::lng_polls_view_results(tr::now))
		: tr::lng_polls_submit_votes(tr::now);
}

int Poll::Footer::countHeight(int innerWidth) const {
	return computeLayout(innerWidth).totalHeight;
}

void Poll::Footer::prepareLinkRippleMask(
		const Layout &layout,
		int outerWidth) const {
	if (!layout.link || layout.compact) {
		resetLinkRipple();
		_linkRippleMask = QImage();
		_linkRippleLayoutSize = QSize();
		_linkRippleMaskSize = QSize();
		_linkRippleShift = 0;
		return;
	}
	const auto linkHeight = layout.totalHeight - layout.topSkip;
	const auto layoutSize = QSize(outerWidth, linkHeight);
	const auto rounding = _owner->adjustedBubbleRounding();
	if (!_linkRippleMask.isNull()
		&& _linkRippleLayoutSize == layoutSize
		&& _linkRippleMaskRounding == rounding) {
		return;
	}
	resetLinkRipple();
	auto mask = _owner->isRoundedInBubbleBottom()
		? static_cast<Message*>(_owner->_parent.get())
			->bottomRippleMask(linkHeight)
		: BottomRippleMask{
			Ui::RippleAnimation::RectMask(layoutSize),
		};
	const auto ratio = style::DevicePixelRatio();
	_linkRippleLayoutSize = layoutSize;
	_linkRippleMaskSize = QSize(
		mask.image.width() / ratio,
		mask.image.height() / ratio);
	_linkRippleMaskRounding = rounding;
	_linkRippleShift = mask.shift;
	_linkRippleMask = std::move(mask.image);
}

QRect Poll::Footer::linkRippleRect(
		const Layout &layout,
		int outerWidth) const {
	return _linkRippleMaskSize.isEmpty()
		? QRect()
		: style::rtlrect(
			-_linkRippleShift,
			layout.topSkip,
			_linkRippleMaskSize.width(),
			_linkRippleMaskSize.height(),
			outerWidth);
}

void Poll::Footer::resetLinkRipple() const {
	_linkRipple.reset();
	_owner->stopRepaintGeneration(_rippleRepaint);
}

void Poll::Footer::draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const {
	const auto layout = computeLayout(innerWidth);
	const auto stm = context.messageStyle();
	prepareLinkRippleMask(layout, outerWidth);
	_owner->recordRepaintGeometry(
		_contentRepaint,
		p,
		context,
		QRegion(QRect(0, 0, outerWidth, layout.totalHeight)));
	_owner->recordRepaintGeometry(
		_rippleRepaint,
		p,
		context,
		QRegion(linkRippleRect(layout, outerWidth)));

	if (_linkRipple && !layout.compact) {
		p.setOpacity(st::historyPollRippleOpacity);
		_linkRipple->paint(
			p,
			left - st::msgPadding.left() - _linkRippleShift,
			layout.topSkip,
			outerWidth,
			&stm->msgWaveformInactive->c);
		if (_linkRipple->empty()) {
			resetLinkRipple();
		}
		p.setOpacity(1.);
	}

	switch (layout.kind) {
	case Layout::Kind::PassiveLabel: {
		p.setPen(stm->msgDateFg);
		if (layout.timerFolded) {
			p.setFont(st::msgDateFont);
			const auto sep = QString::fromUtf8(" \xC2\xB7 ");
			const auto full = _totalVotesLabel.toString()
				+ sep
				+ closeTimerText();
			const auto fullw = st::msgDateFont->width(full);
			p.drawTextLeft(
				left + (innerWidth - fullw) / 2,
				layout.textY,
				outerWidth,
				full,
				fullw);
		} else {
			const auto labelWidth = _totalVotesLabel.maxWidth();
			const auto labelLeft = left
				+ (innerWidth - labelWidth) / 2;
			_totalVotesLabel.drawLeftElided(
				p,
				labelLeft,
				layout.textY,
				labelWidth,
				outerWidth);
		}
		break;
	}
	case Layout::Kind::SaveOption: {
		p.setFont(st::semiboldFont);
		p.setPen(stm->msgFileThumbLinkFg);
		const auto text = tr::lng_polls_add_option_save(tr::now);
		const auto textw = st::semiboldFont->width(text);
		p.drawTextLeft(
			left + (innerWidth - textw) / 2,
			layout.textY,
			outerWidth,
			text,
			textw);
		break;
	}
	case Layout::Kind::AdminVotes: {
		p.setPen(stm->msgFileThumbLinkFg);
		const auto labelWidth = _adminVotesLabel.maxWidth();
		_adminVotesLabel.drawLeft(
			p,
			left + (innerWidth - labelWidth) / 2,
			layout.textY,
			labelWidth,
			outerWidth);
		break;
	}
	case Layout::Kind::AdminBack: {
		p.setPen(stm->msgFileThumbLinkFg);
		const auto backw = _adminBackVoteLabel.maxWidth();
		_adminBackVoteLabel.drawLeft(
			p,
			left + (innerWidth - backw) / 2,
			layout.textY,
			backw,
			outerWidth);
		break;
	}
	case Layout::Kind::LinkButton: {
		p.setFont(st::semiboldFont);
		p.setPen(layout.link
			? stm->msgFileThumbLinkFg
			: stm->msgDateFg);
		const auto string = linkButtonText();
		const auto stringw = st::semiboldFont->width(string);
		p.drawTextLeft(
			left + (innerWidth - stringw) / 2,
			layout.textY,
			outerWidth,
			string,
			stringw);
		break;
	}
	}

	if (layout.timerSeparate) {
		p.setFont(st::msgDateFont);
		p.setPen(stm->msgDateFg);
		const auto timerText = closeTimerText();
		const auto timerw = st::msgDateFont->width(timerText);
		p.drawTextLeft(
			left + (innerWidth - timerw) / 2,
			layout.timerY,
			outerWidth,
			timerText,
			timerw);
	}
}

TextState Poll::Footer::textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const {
	const auto layout = computeLayout(innerWidth);
	TextState result;

	const auto timer = timerRect(layout, left, innerWidth);
	if (!timer.isEmpty() && timer.contains(point)) {
		result.customTooltip = true;
		using Flag = Ui::Text::StateRequest::Flag;
		if (request.flags & Flag::LookupCustomTooltip) {
			result.customTooltipText = langDateTimeFull(
				base::unixtime::parse(_owner->_poll->closeDate));
		}
	}
	if (layout.compact) {
		return result;
	}
	if (point.y() < layout.topSkip
		|| point.y() >= layout.totalHeight) {
		return result;
	}
	_owner->_lastLinkPoint = point;
	result.link = layout.link;
	return result;
}

void Poll::Footer::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (handler == _sendVotesLink
		|| handler == _showResultsLink
		|| handler == _adminVotesLink
		|| handler == _adminBackVoteLink
		|| handler == _saveOptionLink) {
		toggleLinkRipple(pressed);
	}
}

void Poll::Footer::toggleLinkRipple(bool pressed) {
	if (pressed) {
		const auto outerWidth = _owner->width();
		const auto innerWidth = outerWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		const auto layout = computeLayout(innerWidth);
		prepareLinkRippleMask(layout, outerWidth);
		const auto rippleRect = linkRippleRect(layout, outerWidth);
		if (_linkRippleMask.isNull() || rippleRect.isEmpty()) {
			return;
		}
		if (!_linkRipple) {
			const auto generation = _owner->startRepaintGeneration(
				_rippleRepaint);
			const auto weak = base::make_weak(_owner.get());
			_linkRipple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				_linkRippleMask,
				[=] {
					if (const auto strong = weak.get()) {
						strong->repaintGeometry(
							strong->_footerPart->_rippleRepaint,
							generation);
					}
				});
		}
		_linkRipple->add(_owner->_lastLinkPoint - rippleRect.topLeft());
	} else if (_linkRipple) {
		_linkRipple->lastStop();
	}
}

void Poll::Footer::updateTotalVotes() {
	if (_owner->_totalVotes == _owner->_poll->totalVoters
		&& !_totalVotesLabel.isEmpty()) {
		return;
	}
	_owner->_totalVotes = _owner->_poll->totalVoters;
	const auto quiz = _owner->_poll->quiz();
	const auto string = !_owner->_totalVotes
		? (quiz
			? tr::lng_polls_answers_none
			: tr::lng_polls_votes_none)(tr::now)
		: (quiz
			? tr::lng_polls_answers_count
			: tr::lng_polls_votes_count)(
				tr::now,
				lt_count_short,
				_owner->_totalVotes);
	_totalVotesLabel.setText(st::msgDateTextStyle, string);
	_adminVotesLabel.setMarkedText(
		st::semiboldTextStyle,
		tr::lng_polls_admin_votes(
			tr::now,
			lt_count,
			_owner->_totalVotes,
			lt_arrow,
			Ui::Text::IconEmoji(&st::textMoreIconEmoji),
			tr::marked));
}

struct Poll::AddOption : public Poll::Part {
	explicit AddOption(not_null<Poll*> owner);

	int countHeight(int innerWidth) const override;
	void draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const override;
	TextState textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const override;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;

	ClickHandlerPtr _addOptionLink;
	mutable std::unique_ptr<Ui::RippleAnimation> _addOptionRipple;
	mutable QSize _addOptionRippleMaskSize;
	mutable RepaintState _rippleRepaint;

private:
	[[nodiscard]] int rowHeight() const;
	void resetRipple() const;
	void toggleRipple(bool pressed);
};

Poll::AddOption::AddOption(not_null<Poll*> owner)
: Part(owner)
, _addOptionLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		owner.get(),
		[=](ClickContext context) {
			const auto my = context.other.value<ClickHandlerContext>();
			if (const auto delegate = my.elementDelegate
				? my.elementDelegate()
				: nullptr) {
				const auto innerWidth = owner->width()
					- st::msgPadding.left()
					- st::msgPadding.right();
				delegate->elementShowAddPollOption(
					owner->_parent,
					owner->_poll,
					owner->_parent->data()->fullId(),
					owner->addOptionRect(innerWidth));
			}
		}))) {
}

int Poll::AddOption::rowHeight() const {
	return st::historyPollAnswerPaddingNoMedia.top()
		+ st::msgDateFont->height
		+ st::historyPollAnswerPaddingNoMedia.bottom();
}

int Poll::AddOption::countHeight(int innerWidth) const {
	return _owner->canAddOption() ? rowHeight() : 0;
}

void Poll::AddOption::draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const {
	const auto visible = _owner->canAddOption()
		&& !_owner->_addOptionActive;
	const auto maskSize = QSize(outerWidth, rowHeight());
	_owner->recordRepaintGeometry(
		_rippleRepaint,
		p,
		context,
		visible ? QRegion(QRect(QPoint(), maskSize)) : QRegion());
	if (!visible) {
		resetRipple();
		return;
	}
	if (_addOptionRipple && _addOptionRippleMaskSize != maskSize) {
		resetRipple();
	}
	const auto stm = context.messageStyle();
	const auto &padding = st::historyPollAnswerPaddingNoMedia;
	const auto textTop = padding.top();

	if (_addOptionRipple) {
		p.setOpacity(st::historyPollRippleOpacity);
		_addOptionRipple->paint(
			p,
			left - st::msgPadding.left(),
			0,
			outerWidth,
			&stm->msgWaveformInactive->c);
		if (_addOptionRipple->empty()) {
			resetRipple();
		}
		p.setOpacity(1.);
	}

	const auto color = stm->msgDateFg->c;
	const auto &icon = st::pollBoxOutlinePollAddIcon;
	const auto &radio = st::historyPollRadio;
	const auto iconLeft = left
		+ (radio.diameter - icon.width()) / 2;
	const auto iconTop = textTop
		+ (st::msgDateFont->height - icon.height()) / 2;
	icon.paint(p, iconLeft, iconTop, outerWidth, color);

	p.setFont(st::normalFont);
	p.setPen(stm->msgDateFg);
	const auto text = tr::lng_polls_add_option(tr::now);
	const auto textw = st::normalFont->width(text);
	p.drawTextLeft(
		left + st::historyPollAnswerPadding.left(),
		textTop,
		outerWidth,
		text,
		textw);
}

TextState Poll::AddOption::textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const {
	TextState result;
	if (!_owner->canAddOption()) {
		return result;
	}
	const auto h = rowHeight();
	if (point.y() >= 0 && point.y() < h) {
		_owner->_lastLinkPoint = point;
		result.link = _addOptionLink;
	}
	return result;
}

void Poll::AddOption::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (handler == _addOptionLink) {
		toggleRipple(pressed);
	}
}

void Poll::AddOption::resetRipple() const {
	_addOptionRipple.reset();
	_addOptionRippleMaskSize = QSize();
	_owner->stopRepaintGeneration(_rippleRepaint);
}

void Poll::AddOption::toggleRipple(bool pressed) {
	if (pressed) {
		const auto outerWidth = _owner->width();
		const auto h = rowHeight();
		const auto maskSize = QSize(outerWidth, h);
		if (_addOptionRipple && _addOptionRippleMaskSize != maskSize) {
			resetRipple();
		}
		if (!_addOptionRipple) {
			auto mask = Ui::RippleAnimation::RectMask(maskSize);
			const auto generation = _owner->startRepaintGeneration(
				_rippleRepaint);
			const auto weak = base::make_weak(_owner.get());
			_addOptionRippleMaskSize = maskSize;
			_addOptionRipple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				std::move(mask),
				[=] {
					if (const auto strong = weak.get()) {
						strong->repaintGeometry(
							strong->_addOptionPart->_rippleRepaint,
							generation);
					}
				});
		}
		_addOptionRipple->add(_owner->_lastLinkPoint);
	} else if (_addOptionRipple) {
		_addOptionRipple->lastStop();
	}
}

struct Poll::Header : public Poll::Part {
	enum class TextPart {
		Description,
		Question,
		Solution,
	};

	struct TextRepaint {
		uint64 generation = 0;
		QRegion current;
		QRegion stale;
		uint32 pending : 1 = 0;
		uint32 known : 1 = 0;
	};

	explicit Header(not_null<Poll*> owner)
	: Part(owner)
	, _description(st::msgMinWidth / 2)
	, _question(st::msgMinWidth / 2)
	, _attachedMedia(std::make_unique<AttachedMedia>()) {
	}

	int countHeight(int innerWidth) const override;
	void draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const override;
	TextState textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const override;
	void clickHandlerActiveChanged(
		const ClickHandlerPtr &handler,
		bool active) override;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;
	bool hasHeavyPart() const override;
	void unloadHeavyPart() override;
	uint16 selectionLength() const override;

	void updateDescription();
	[[nodiscard]] Fn<void()> textRepaintCallback(TextPart part);
	void resetTextRepaint(TextPart part);
	void repaintText(TextPart part, uint64 generation) const;
	void finishTextRepaint(
		TextPart part,
		const Painter &p,
		const PaintContext &context,
		QRegion region,
		bool geometryKnown) const;
	void invalidateTextRepaints() const;
	void invalidateTextRepaint(TextRepaint &repaint) const;
	void repaintTextRegion(const QRegion &region) const;
	[[nodiscard]] TextRepaint &textRepaint(TextPart part) const;
	void updateAttachedMedia();
	[[nodiscard]] int countTopContentSkip(int pollWidth = 0) const;
	[[nodiscard]] int countTopMediaHeight(int pollWidth = 0) const;
	[[nodiscard]] int countAttachHeight(int pollWidth = 0) const;
	[[nodiscard]] QRect countTopMediaRect(int top) const;
	[[nodiscard]] Ui::BubbleRounding topMediaRounding() const;
	void validateTopMediaCache(QSize size) const;
	[[nodiscard]] int countDescriptionHeight(int innerWidth) const;
	[[nodiscard]] int countQuestionTop(
		int innerWidth,
		int pollWidth = 0) const;
	[[nodiscard]] uint16 solutionSelectionLength() const;
	[[nodiscard]] TextSelection toSolutionSelection(
		TextSelection selection) const;
	[[nodiscard]] TextSelection fromSolutionSelection(
		TextSelection selection) const;
	[[nodiscard]] TextSelection toQuestionSelection(
		TextSelection selection) const;
	[[nodiscard]] TextSelection fromQuestionSelection(
		TextSelection selection) const;
	void updateRecentVoters();
	void paintRecentVoters(
		Painter &p,
		int left,
		int top,
		const PaintContext &context) const;
	void paintShowSolution(
		Painter &p,
		int right,
		int top,
		const PaintContext &context) const;
	void showSolution() const;
	void solutionToggled(
		bool solutionShown,
		anim::type animated = anim::type::normal) const;
	[[nodiscard]] bool canShowSolution() const;
	[[nodiscard]] bool inShowSolution(
		QPoint point,
		int right,
		int top) const;
	void updateSolutionText();
	void updateSolutionMedia();
	[[nodiscard]] int countSolutionBlockHeight(int innerWidth) const;
	[[nodiscard]] int countSolutionMediaHeight(int mediaWidth) const;
	void paintSolutionBlock(
		Painter &p,
		int left,
		int top,
		int paintw,
		const PaintContext &context) const;

	Ui::Text::String _description;
	Ui::Text::String _question;
	Ui::Text::String _subtitle;
	std::unique_ptr<AttachedMedia> _attachedMedia;
	std::unique_ptr<Media> _attachedMediaAttach;
	mutable QImage _attachedMediaCache;
	mutable Ui::BubbleRounding _attachedMediaCacheRounding;
	mutable bool _attachedMediaCacheBlurred = false;
	std::vector<RecentVoter> _recentVoters;
	QImage _recentVotersImage;
	mutable ClickHandlerPtr _showSolutionLink;
	Ui::Text::String _solutionText;
	mutable TextRepaint _descriptionRepaint;
	mutable TextRepaint _questionRepaint;
	mutable TextRepaint _solutionRepaint;
	mutable RepaintState _solutionButtonRepaint;
	mutable ClickHandlerPtr _closeSolutionLink;
	std::unique_ptr<SolutionMedia> _solutionMedia;
	std::unique_ptr<Media> _solutionAttach;
	mutable Ui::Animations::Simple _solutionButtonAnimation;
	mutable bool _solutionShown = false;
	mutable bool _solutionButtonVisible = false;
	mutable QImage _userpicCircleCache;
};

int Poll::Header::countHeight(int innerWidth) const {
	const auto pollWidth = innerWidth
		+ st::msgPadding.left()
		+ st::msgPadding.right();
	return countQuestionTop(innerWidth, pollWidth)
		+ _question.countHeight(innerWidth)
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip;
}

void Poll::Header::draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const {
	const auto stm = context.messageStyle();
	auto tshift = countTopContentSkip();
	auto descriptionRepaintRegion = QRegion();
	auto descriptionRepaintKnown = context.hasElementPainter(p);

	if (const auto mediaHeight = countTopMediaHeight()) {
		if (_attachedMediaAttach) {
			const auto sideSkip = st::historyPollMediaSideSkip;
			_attachedMediaAttach->setBubbleRounding(
				topMediaRounding());
			p.translate(sideSkip, tshift);
			_attachedMediaAttach->draw(
				p,
				context.translated(-sideSkip, -tshift)
					.withSelection(TextSelection()));
			p.translate(-sideSkip, -tshift);
		} else {
			const auto target = countTopMediaRect(tshift);
			p.setPen(Qt::NoPen);
			p.setBrush(stm->msgFileBg);
			PainterHighQualityEnabler hq(p);
			if (_attachedMedia->kind
					== PollThumbnailKind::Emoji) {
				p.drawRoundedRect(
					target,
					st::roundRadiusLarge,
					st::roundRadiusLarge);
				const auto image
					= _attachedMedia->thumbnail->image(
						std::max(target.width(), target.height()));
				if (!image.isNull()) {
					const auto source = QRectF(
						QPointF(),
						QSizeF(image.size()));
					const auto kx = target.width() / source.width();
					const auto ky = target.height() / source.height();
					const auto scale = std::min(kx, ky);
					const auto imageSize = QSizeF(
						source.width() * scale,
						source.height() * scale);
					const auto geometry = QRectF(
						target.x()
							+ (target.width()
								- imageSize.width()) / 2.,
						target.y()
							+ (target.height()
								- imageSize.height()) / 2.,
						imageSize.width(),
						imageSize.height());
					p.save();
					auto path = QPainterPath();
					path.addRoundedRect(
						target,
						st::roundRadiusLarge,
						st::roundRadiusLarge);
					p.setClipPath(path);
					p.drawImage(geometry, image, source);
					p.restore();
				}
			} else {
				validateTopMediaCache(target.size());
				if (!_attachedMediaCache.isNull()) {
					p.drawImage(target.topLeft(),
						_attachedMediaCache);
				}
			}
		}
		tshift += mediaHeight + st::historyPollMediaSkip;
	}

	if (const auto descriptionHeight
			= countDescriptionHeight(innerWidth)) {
		p.setPen(stm->historyTextFg);
		_owner->_parent->prepareCustomEmojiPaint(
			p, context, _description);
		auto customEmojiRepaintBounds
			= Ui::Text::CustomEmojiRepaintBounds();
		_description.draw(p, {
			.position = { left, tshift },
			.outerWidth = outerWidth,
			.availableWidth = innerWidth,
			.spoiler = Ui::Text::DefaultSpoilerCache(),
			.now = context.now,
			.pausedEmoji = context.paused,
			.pausedSpoiler = context.paused,
			.selection = context.selection,
			.useFullWidth = true,
			.customEmojiRepaintBounds = &customEmojiRepaintBounds,
		});
		descriptionRepaintKnown = descriptionRepaintKnown
			&& AddPollTextRepaintRect(
				descriptionRepaintRegion,
				p,
				context,
				_description,
				QRect(left, tshift, innerWidth, descriptionHeight),
				customEmojiRepaintBounds,
				true);
		tshift += descriptionHeight + st::historyPollDescriptionSkip;
	}
	finishTextRepaint(
		TextPart::Description,
		p,
		context,
		std::move(descriptionRepaintRegion),
		descriptionRepaintKnown);

	if (const auto solutionHeight
			= countSolutionBlockHeight(innerWidth)) {
		paintSolutionBlock(
			p, left, tshift, innerWidth, context);
		tshift += solutionHeight + st::historyPollExplanationSkip;
	} else {
		finishTextRepaint(
			TextPart::Solution,
			p,
			context,
			QRegion(),
			context.hasElementPainter(p));
	}

	auto questionRepaintRegion = QRegion();
	auto questionRepaintKnown = context.hasElementPainter(p);
	const auto questionHeight = _question.countHeight(innerWidth);
	p.setPen(stm->historyTextFg);
	_owner->_parent->prepareCustomEmojiPaint(p, context, _question);
	auto questionCustomEmojiRepaintBounds
		= Ui::Text::CustomEmojiRepaintBounds();
	_question.draw(p, {
		.position = { left, tshift },
		.outerWidth = outerWidth,
		.availableWidth = innerWidth,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.pausedEmoji = context.paused,
		.pausedSpoiler = context.paused,
		.selection = toQuestionSelection(context.selection),
		.customEmojiRepaintBounds = &questionCustomEmojiRepaintBounds,
	});
	questionRepaintKnown = questionRepaintKnown
		&& AddPollTextRepaintRect(
			questionRepaintRegion,
			p,
			context,
			_question,
			QRect(left, tshift, innerWidth, questionHeight),
			questionCustomEmojiRepaintBounds);
	finishTextRepaint(
		TextPart::Question,
		p,
		context,
		std::move(questionRepaintRegion),
		questionRepaintKnown);
	tshift += questionHeight
		+ st::historyPollSubtitleSkip;

	p.setPen(stm->msgDateFg);
	_subtitle.drawLeftElided(
		p, left, tshift, innerWidth, outerWidth);
	paintRecentVoters(
		p, left + _subtitle.maxWidth(), tshift, context);
	paintShowSolution(
		p, left + innerWidth, tshift, context);
}

TextState Poll::Header::textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const {
	TextState result(_owner->_parent);
	auto tshift = countTopContentSkip();

	if (const auto mediaHeight = countTopMediaHeight()) {
		if (_attachedMediaAttach) {
			const auto sideSkip = st::historyPollMediaSideSkip;
			if (QRect(
					sideSkip,
					tshift,
					_attachedMediaAttach->width(),
					mediaHeight).contains(point)) {
				result = _attachedMediaAttach->textState(
					point - QPoint(sideSkip, tshift),
					request);
				SetTextStatePosition(&result, 0, false);
				return result;
			}
		} else if (_attachedMedia
			&& _attachedMedia->handler
			&& QRect(0, tshift, outerWidth, mediaHeight)
				.contains(point)) {
			result.link = _attachedMedia->handler;
			return result;
		}
		tshift += mediaHeight + st::historyPollMediaSkip;
	}

	auto symbolAdd = uint16(0);
	if (const auto descriptionHeight
			= countDescriptionHeight(innerWidth)) {
		if (QRect(left, tshift, innerWidth, descriptionHeight)
				.contains(point)) {
			result = TextState(
				_owner->_parent,
				_description.getStateLeft(
					point - QPoint(left, tshift),
					innerWidth,
					outerWidth,
					request.forText()));
			return result;
		}
		if (point.y() >= tshift + descriptionHeight) {
			symbolAdd += _description.length();
		}
		tshift += descriptionHeight + st::historyPollDescriptionSkip;
	}

	if (const auto solutionHeight
			= countSolutionBlockHeight(innerWidth)) {
		if (QRect(left, tshift, innerWidth, solutionHeight)
				.contains(point)) {
			const auto &qst = st::historyPagePreview;
			const auto innerLeft = left + qst.padding.left();
			const auto innerRight = left
				+ innerWidth
				- qst.padding.right();
			const auto closeArea
				= st::historyPollExplanationCloseSize;
			const auto closeLeft = innerRight - closeArea;
			const auto closeTop = tshift + qst.padding.top();
			if (QRect(
				closeLeft,
				closeTop,
				closeArea,
				st::semiboldFont->height).contains(point)) {
				result.link = _closeSolutionLink;
				return result;
			}
			const auto textTop = tshift
				+ qst.padding.top()
				+ st::semiboldFont->height
				+ st::historyPollExplanationTitleSkip;
			const auto textWidth = innerRight - innerLeft;
			const auto textHeight
				= _solutionText.countHeight(textWidth);
			if (QRect(
				innerLeft,
				textTop,
				textWidth,
				textHeight).contains(point)) {
				result = TextState(
					_owner->_parent,
					_solutionText.getStateLeft(
						point - QPoint(innerLeft, textTop),
						textWidth,
						outerWidth,
						request.forText()));
				AddTextStateOffset(&result, symbolAdd);
				return result;
			}
			if (_solutionAttach) {
				if (const auto mh
						= countSolutionMediaHeight(
							textWidth)) {
					const auto mediaTop = textTop
						+ textHeight
						+ st::historyPollExplanationMediaSkip;
					const auto isDocument = _solutionMedia
						&& (_solutionMedia->kind
								== PollThumbnailKind::Document
							|| _solutionMedia->kind
								== PollThumbnailKind::Audio);
					const auto isThumbed = isDocument
						&& _owner->_poll->solutionMedia.document
						&& _owner->_poll->solutionMedia.document
							->hasThumbnail()
						&& !_owner->_poll->solutionMedia.document
							->isSong();
					const auto &fileSt = isThumbed
						? st::msgFileThumbLayout
						: st::msgFileLayout;
					const auto shift = isDocument
						? fileSt.padding.left()
						: 0;
					const auto mediaLeft = innerLeft - shift;
					if (QRect(
							mediaLeft,
							mediaTop,
							_solutionAttach->width(),
							mh).contains(point)) {
						result = _solutionAttach->textState(
							point - QPoint(mediaLeft, mediaTop),
							request);
						SetTextStatePosition(&result, 0, false);
					}
				}
			}
			return result;
		}
		if (point.y() >= tshift + solutionHeight) {
			symbolAdd += _solutionText.length();
		}
		tshift += solutionHeight + st::historyPollExplanationSkip;
	}

	const auto questionH = _question.countHeight(innerWidth);
	if (QRect(left, tshift, innerWidth, questionH).contains(point)) {
		result = TextState(
			_owner->_parent,
			_question.getState(
				point - QPoint(left, tshift),
				innerWidth,
				request.forText()));
		AddTextStateOffset(&result, symbolAdd);
		return result;
	}
	if (point.y() >= tshift + questionH) {
		symbolAdd += _question.length();
	}
	tshift += questionH + st::historyPollSubtitleSkip;
	if (inShowSolution(
			point, left + innerWidth, tshift)) {
		result.link = _showSolutionLink;
		return result;
	}

	return result;
}

void Poll::Header::clickHandlerActiveChanged(
		const ClickHandlerPtr &handler,
		bool active) {
	if (_attachedMediaAttach) {
		_attachedMediaAttach->clickHandlerActiveChanged(
			handler, active);
	}
}

void Poll::Header::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (_attachedMediaAttach) {
		_attachedMediaAttach->clickHandlerPressedChanged(
			handler, pressed);
	}
	if (_solutionAttach) {
		_solutionAttach->clickHandlerPressedChanged(
			handler, pressed);
	}
}

bool Poll::Header::hasHeavyPart() const {
	for (const auto &recent : _recentVoters) {
		if (!recent.userpic.null()) {
			return true;
		}
	}
	return false;
}

void Poll::Header::unloadHeavyPart() {
	for (auto &recent : _recentVoters) {
		recent.userpic = {};
	}
	_description.unloadPersistentAnimation();
	_question.unloadPersistentAnimation();
	_solutionText.unloadPersistentAnimation();
}

uint16 Poll::Header::selectionLength() const {
	return _owner->fullSelectionLength();
}

struct Poll::Options : public Poll::Part {
	using Part::Part;
	enum class FeedbackPart {
		Toggle,
		Ripple,
	};
	struct FeedbackRepaints {
		QByteArray option;
		RepaintState toggle;
		RepaintState ripple;
	};

	struct TextRepaint {
		QByteArray option;
		uint64 generation = 0;
		QRegion repaintRegion;
		QRegion collectedRepaintRegion;
		QRegion staleRepaintRegion;
		bool repaintPending = false;
		bool collectingRepaintRegion = false;
		bool repaintKnown = false;
		bool collectedRepaintKnown = false;
	};

	struct ThumbnailRepaint {
		QByteArray option;
		const Ui::DynamicImage *image = nullptr;
		QRegion repaintRegion;
		QRegion collectedRepaintRegion;
		QRegion staleRepaintRegion;
		bool repaintPending = false;
		bool collectingRepaintRegion = false;
	};

	int countHeight(int innerWidth) const override;
	void draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const override;
	TextState textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const override;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;
	bool hasHeavyPart() const override;

	void checkSendingAnimation() const;
	void unloadHeavyPart() override;

	[[nodiscard]] int countAnswerContentWidth(
		const Answer &answer,
		int innerWidth) const;
	[[nodiscard]] int countVotesExtraHeight(
		const Answer &answer,
		int textWidth) const;
	[[nodiscard]] int countAnswerHeight(
		const Answer &answer,
		int innerWidth) const;
	void resetAnswersAnimation() const;
	void repaintAnswersAnimation() const;
	void beginAnswersAnimationPaint(
		const Painter &p,
		const PaintContext &context) const;
	void recordAnswersAnimationRect(
		const Painter &p,
		const PaintContext &context,
		QRect rect) const;
	void finishAnswersAnimationPaint() const;
	void repaintAnswersAnimationRegion(const QRegion &region) const;
	void radialAnimationCallback() const;
	void beginSendingAnimationPaint(
		const Painter &p,
		const PaintContext &context) const;
	void recordSendingAnimationRect(
		const Painter &p,
		const PaintContext &context,
		QRectF rect) const;
	void finishSendingAnimationPaint() const;
	void repaintSendingAnimationRegion(const QRegion &region) const;
	void resetSendingAnimation() const;
	void fillAnswerData(
		Answer &answer,
		const PollAnswer &original);
	void answerTextUpdated(
		const QByteArray &option,
		uint64 generation) const;
	void invalidateAnswerTextRepaints();
	void syncAnswerTextRepaints();
	void beginAnswerTextPaint(
		const Painter &p,
		const PaintContext &context) const;
	void recordAnswerTextRect(
		const Painter &p,
		const PaintContext &context,
		const Answer &answer,
		QRect rect,
		const Ui::Text::CustomEmojiRepaintBounds
			&customEmojiRepaintBounds) const;
	void finishAnswerTextPaint() const;
	void repaintAnswerTextRegion(const QRegion &region) const;
	void subscribeToThumbnailUpdates(
		not_null<Ui::DynamicImage*> image,
		PollThumbnailKind kind,
		QByteArray option) const;
	void thumbnailUpdated(
		PollThumbnailKind kind,
		const QByteArray &option,
		const Ui::DynamicImage *image) const;
	void invalidateAnimatedThumbnailRepaints();
	void syncAnimatedThumbnailRepaints();
	void beginAnimatedThumbnailPaint(
		const Painter &p,
		const PaintContext &context) const;
	void recordAnimatedThumbnailRect(
		const Painter &p,
		const PaintContext &context,
		const Answer &answer,
		QRect rect) const;
	void finishAnimatedThumbnailPaint() const;
	void repaintAnimatedThumbnailRegion(const QRegion &region) const;
	void recordAnswerFeedbackRect(
		const Painter &p,
		const PaintContext &context,
		const Answer &answer,
		FeedbackPart part,
		QRect rect) const;
	[[nodiscard]] FeedbackRepaints &ensureFeedbackRepaints(
		const QByteArray &option) const;
	[[nodiscard]] FeedbackRepaints *findFeedbackRepaints(
		const QByteArray &option) const;
	[[nodiscard]] RepaintState &feedbackRepaint(
		FeedbackRepaints &repaints,
		FeedbackPart part) const;
	[[nodiscard]] uint64 resetFeedbackRepaint(
		const QByteArray &option,
		FeedbackPart part) const;
	void repaintAnswerFeedback(
		const QByteArray &option,
		FeedbackPart part,
		uint64 generation) const;
	void repaintAnswerFeedback(
		const QByteArray &option,
		FeedbackPart part) const;
	void invalidateFeedbackRepaints() const;
	void syncFeedbackRepaints() const;
	void finishFeedbackRepaints() const;
	void resetAnswerRipple(const Answer &answer) const;
	int paintAnswer(
		Painter &p,
		const Answer &answer,
		const AnswerAnimation *animation,
		int left,
		int top,
		int width,
		int outerWidth,
		const PaintContext &context) const;
	void paintRadio(
		Painter &p,
		const Answer &answer,
		int left,
		int top,
		const PaintContext &context) const;
	void paintPercent(
		Painter &p,
		const QString &percent,
		int percentWidth,
		int left,
		int top,
		int topPadding,
		int outerWidth,
		const PaintContext &context) const;
	void paintFilling(
		Painter &p,
		bool chosen,
		bool correct,
		float64 filling,
		int left,
		int top,
		int topPadding,
		int width,
		int contentWidth,
		int contentHeight,
		const PaintContext &context) const;
	[[nodiscard]] bool checkAnimationStart() const;
	[[nodiscard]] bool answerVotesChanged() const;
	void saveStateInAnimation() const;
	void startAnswersAnimation() const;
	void toggleRipple(Answer &answer, bool pressed);
	void clearSelected();
	void toggleMultiOption(const QByteArray &option);
	void sendMultiOptions();
	void showResults();
	void showAnswerVotesTooltip(const QByteArray &option);
	void checkQuizAnswered();
	[[nodiscard]] ClickHandlerPtr createAnswerClickHandler(
		const Answer &answer);
	void updateAnswers();
	void updateAnswerVotes();
	void updateAnswerVotesFromOriginal(
		Answer &answer,
		const PollAnswer &original,
		int percent,
		int maxVotes,
		bool showPercent);

	mutable std::vector<TextRepaint> _textRepaints;
	mutable std::vector<ThumbnailRepaint> _thumbnailRepaints;
	mutable std::vector<FeedbackRepaints> _feedbackRepaints;
	std::vector<Answer> _answers;
	mutable std::unique_ptr<AnswersAnimation> _answersAnimation;
	mutable std::unique_ptr<SendingAnimation> _sendingAnimation;
	mutable QImage _fillingIconCache;
	uint64 _textGeneration = 0;
	bool _anyAnswerHasMedia = false;
	bool _hasSelected = false;
	bool _votedFromHere = false;
};

int Poll::Options::countHeight(int innerWidth) const {
	auto result = ranges::accumulate(ranges::views::all(
		_answers
	) | ranges::views::transform([&](const Answer &answer) {
		return countAnswerHeight(answer, innerWidth);
	}), 0);
	if (_owner->canAddOption()) {
		result += (st::historyPollChoiceRight.height()
			- st::historyPollFillingHeight) / 2;
	}
	return result;
}

void Poll::Options::draw(
		Painter &p,
		int left,
		int innerWidth,
		int outerWidth,
		const PaintContext &context) const {
	checkSendingAnimation();

	const auto progress = _answersAnimation
		? _answersAnimation->progress.value(1.)
		: 1.;
	if (progress == 1.) {
		resetAnswersAnimation();
	}
	beginAnswersAnimationPaint(p, context);
	const auto choiceOverhang = (st::historyPollChoiceRight.height()
		- st::historyPollFillingHeight) / 2;
	recordAnswersAnimationRect(
		p,
		context,
		QRect(0, 0, outerWidth, countHeight(innerWidth)).marginsAdded(
			QMargins(
				st::lineWidth,
				st::lineWidth,
				st::lineWidth,
				choiceOverhang + 2 * st::lineWidth)));
	beginSendingAnimationPaint(p, context);
	beginAnswerTextPaint(p, context);
	beginAnimatedThumbnailPaint(p, context);

	auto tshift = 0;
	auto &&answers = ranges::views::zip(
		_answers,
		ranges::views::ints(0, int(_answers.size())));
	for (const auto &[answer, index] : answers) {
		const auto animation = _answersAnimation
			? &_answersAnimation->data[index]
			: nullptr;
		if (animation) {
			animation->percent.update(progress, anim::linear);
			animation->filling.update(
				progress,
				_owner->showVotes()
					? anim::easeOutCirc
					: anim::linear);
			animation->opacity.update(progress, anim::linear);
		}
		const auto height = paintAnswer(
			p,
			answer,
			animation,
			left,
			tshift,
			innerWidth,
			outerWidth,
			context);
		tshift += height;
	}
	finishAnimatedThumbnailPaint();
	finishAnswerTextPaint();
	finishSendingAnimationPaint();
	finishAnswersAnimationPaint();
	if (context.hasElementPainter(p)) {
		finishFeedbackRepaints();
	}
}

TextState Poll::Options::textState(
		QPoint point,
		int left,
		int innerWidth,
		int outerWidth,
		StateRequest request) const {
	TextState result;
	const auto can = _owner->canVote();
	const auto show = _owner->showVotes();

	auto tshift = 0;
	for (const auto &answer : _answers) {
		const auto height = countAnswerHeight(answer, innerWidth);
		if (point.y() >= tshift && point.y() < tshift + height) {
			const auto media = answer.thumbnail
				? PollAnswerMediaSize()
				: 0;
			if (media
				&& answer.mediaHandler
				&& QRect(
					left + innerWidth
						- st::historyPollAnswerPadding.right()
						- media,
					tshift + (answer.thumbnail
						? st::historyPollAnswerPadding
						: st::historyPollAnswerPaddingNoMedia).top(),
					media,
					media).contains(point)) {
				result.link = answer.mediaHandler;
			} else {
				const auto &answerPadding = answer.thumbnail
					? st::historyPollAnswerPadding
					: st::historyPollAnswerPaddingNoMedia;
				const auto aleft = left
					+ st::historyPollAnswerPadding.left();
				const auto atop = tshift + answerPadding.top();
				const auto textWidth = countAnswerContentWidth(
					answer,
					innerWidth);
				const auto textState = answer.text.getStateLeft(
					point - QPoint(aleft, atop),
					textWidth,
					outerWidth,
					request.forText());
				if (textState.link) {
					result.link = textState.link;
				} else {
					if (can) {
						_owner->_lastLinkPoint = point;
					}
					result.link = answer.handler;
				}
			}
			if (!can && show) {
				result.customTooltip = true;
				using Flag = Ui::Text::StateRequest::Flag;
				if (request.flags & Flag::LookupCustomTooltip) {
					const auto quiz = _owner->_poll->quiz();
					result.customTooltipText = answer.votes
						? (quiz
							? tr::lng_polls_answers_count
							: tr::lng_polls_votes_count)(
								tr::now,
								lt_count_decimal,
								answer.votes)
						: (quiz
							? tr::lng_polls_answers_none
							: tr::lng_polls_votes_none)(tr::now);
				}
			}
			return result;
		}
		tshift += height;
	}
	return result;
}

void Poll::Options::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	const auto i = ranges::find(
		_answers,
		handler,
		&Answer::handler);
	if (i != end(_answers)) {
		if (_owner->canVote()) {
			toggleRipple(*i, pressed);
		}
	}
}

bool Poll::Options::hasHeavyPart() const {
	for (const auto &answer : _answers) {
		if (!answer.recentVotersImage.isNull()) {
			return true;
		}
	}
	return false;
}

void Poll::Options::unloadHeavyPart() {
	for (auto &answer : _answers) {
		answer.text.unloadPersistentAnimation();
		for (auto &recent : answer.recentVoters) {
			recent.view = {};
		}
		answer.recentVotersImage = QImage();
	}
}

template <typename Callback>
Poll::SendingAnimation::SendingAnimation(
	const QByteArray &option,
	Callback &&callback)
: option(option)
, animation(
	std::forward<Callback>(callback),
	st::historyPollRadialAnimation) {
}

Poll::Answer::Answer() : text(st::msgMinWidth / 2) {
}

void Poll::Options::fillAnswerData(
		Answer &answer,
		const PollAnswer &original) {
	const auto poll = _owner->_poll;
	answer.chosen = original.chosen;
	answer.correct = poll->quiz() ? original.correct : answer.chosen;
	if (!answer.text.isEmpty()
		&& answer.text.toTextWithEntities() == original.text) {
		return;
	}
	const auto option = answer.option;
	const auto generation = ++_textGeneration;
	answer.textGeneration = generation;
	answer.text.setMarkedText(
		st::historyPollAnswerStyle,
		original.text,
		Ui::WebpageTextTitleOptions(),
		Core::TextContext({
			.session = &poll->session(),
			.repaint = [=] {
				answerTextUpdated(option, generation);
			},
			.customEmojiLoopLimit = 2,
		}));
}

void Poll::Answer::fillMedia(
		not_null<PollData*> poll,
		const PollAnswer &original,
		Window::SessionController::MessageContext messageContext,
		not_null<Options*> options,
		Fn<bool()> paused) {
	const auto updated = MakePollThumbnail(
		poll,
		original,
		messageContext,
		paused);
	const auto same = (updated.kind == thumbnailKind)
		&& (updated.id == thumbnailId)
		&& (updated.rounded == thumbnailRounded);
	if (same) {
		return;
	}
	if (thumbnail) {
		thumbnail->subscribeToUpdates(nullptr);
	}
	thumbnail = updated.thumbnail;
	mediaHandler = updated.handler;
	thumbnailRounded = updated.rounded;
	thumbnailIsVideo = updated.isVideo;
	thumbnailKind = updated.kind;
	thumbnailId = updated.id;
	if (thumbnail) {
		options->subscribeToThumbnailUpdates(
			thumbnail.get(),
			thumbnailKind,
			option);
	}
}

Poll::Footer::Footer(not_null<Poll*> owner)
: Part(owner)
, _showResultsLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		owner,
		[=] { owner->_optionsPart->showResults(); })))
, _sendVotesLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		owner,
		[=] { owner->_optionsPart->sendMultiOptions(); })))
, _adminVotesLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		owner,
		[=] {
			if (owner->_flags & PollData::Flag::PublicVotes) {
				owner->_optionsPart->showResults();
			} else {
				owner->_optionsPart->saveStateInAnimation();
				owner->_adminShowResults = true;
				owner->_optionsPart->updateAnswerVotes();
				owner->_optionsPart->startAnswersAnimation();
				owner->history()->owner().requestViewResize(owner->_parent);
			}
		})))
, _adminBackVoteLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		owner,
		[=] {
			owner->_optionsPart->saveStateInAnimation();
			owner->_adminShowResults = false;
			owner->_optionsPart->updateAnswerVotes();
			owner->_optionsPart->startAnswersAnimation();
			owner->history()->owner().requestViewResize(owner->_parent);
		})))
, _saveOptionLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		owner,
		[=](ClickContext context) {
			const auto my = context.other.value<ClickHandlerContext>();
			if (const auto delegate = my.elementDelegate
				? my.elementDelegate()
				: nullptr) {
				delegate->elementSubmitAddPollOption(
					owner->_parent->data()->fullId());
			}
		})))
, _closeTimer([=] { owner->repaint(); }) {
}

Poll::Poll(
	not_null<Element*> parent,
	not_null<PollData*> poll,
	const TextWithEntities &consumed)
: Media(parent)
, _poll(poll)
{
	_headerPart = std::make_unique<Header>(this);
	_optionsPart = std::make_unique<Options>(this);
	_addOptionPart = std::make_unique<AddOption>(this);
	_footerPart = std::make_unique<Footer>(this);
	if (!consumed.text.isEmpty()) {
		_headerPart->updateDescription();
	}
	history()->owner().registerPollView(_poll, _parent);
}

void Poll::recordRepaintGeometry(
		RepaintState &repaint,
		QRegion region,
		bool known) const {
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.current = known ? std::move(region) : QRegion();
	repaint.known = known;
	if (!known) {
		repaint.stale = previous;
		if (stale.isEmpty() && !previous.isEmpty()) {
			repaint.pending = true;
			this->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = true;
	repaintRegion(previous.united(repaint.current));
}

void Poll::recordRepaintGeometry(
		RepaintState &repaint,
		const Painter &p,
		const PaintContext &context,
		const QRegion &rects) const {
	if (!context.hasElementPainter(p)) {
		return;
	}
	auto region = QRegion();
	auto known = true;
	for (const auto &rect : rects) {
		const auto mapped = context.mapToElement(p, QRectF(rect));
		if (!mapped) {
			known = false;
			break;
		} else if (mapped->isEmpty()) {
			continue;
		}
		region += *mapped;
	}
	recordRepaintGeometry(repaint, std::move(region), known);
}

void Poll::recordAnimationRepaintGeometry(
		RepaintState &repaint,
		QRegion region,
		bool known) const {
	if (repaint.generation || !repaint.stale.isEmpty()) {
		recordRepaintGeometry(repaint, std::move(region), known);
	} else {
		repaint.current = known ? std::move(region) : QRegion();
		repaint.stale = QRegion();
		repaint.pending = false;
		repaint.known = known;
	}
}

void Poll::invalidateRepaintGeometry(RepaintState &repaint) const {
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.known = false;
}

void Poll::repaintGeometry(RepaintState &repaint) const {
	if (repaint.pending
		|| (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = true;
	if (repaint.known) {
		repaintRegion(repaint.current);
	} else {
		this->repaint();
	}
}

void Poll::repaintGeometry(
		RepaintState &repaint,
		uint64 generation) const {
	if (generation != repaint.generation) {
		return;
	}
	repaintGeometry(repaint);
}

uint64 Poll::startRepaintGeneration(RepaintState &repaint) const {
	repaint.pending = false;
	repaint.generation = ++_nextRepaintGeneration;
	return repaint.generation;
}

void Poll::stopRepaintGeneration(RepaintState &repaint) const {
	repaint.generation = 0;
	repaint.pending = false;
}

void Poll::repaintRegion(const QRegion &region) const {
	_parent->repaint(region);
}

void Poll::invalidateFiniteRepaintGeometries() const {
	invalidateRepaintGeometry(_fireworksRepaint);
	invalidateRepaintGeometry(_wrongAnswerRepaint);
	invalidateRepaintGeometry(_headerPart->_solutionButtonRepaint);
	invalidateRepaintGeometry(_addOptionPart->_rippleRepaint);
	invalidateRepaintGeometry(_footerPart->_contentRepaint);
	invalidateRepaintGeometry(_footerPart->_rippleRepaint);
	_optionsPart->invalidateFeedbackRepaints();
}

void Poll::rememberElementPaint(
		const Painter &p,
		const PaintContext &context) const {
	_lastDrawPaintDevice = p.device();
	_lastDrawCanonical = context.hasElementPainter(p);
	if (!_lastDrawCanonical) {
		return;
	}
	_elementPaintDevice = p.device();
	_elementTransform = context.elementTransform;
}

std::optional<QRect> Poll::mapCurrentPaintToElement(
		const Painter &p,
		QRectF rect) const {
	if (!_elementTransform || _elementPaintDevice != p.device()) {
		return std::nullopt;
	}
	if (rect.isEmpty()) {
		return Ui::DamageRect(rect);
	}
	auto invertible = false;
	const auto inverted = _elementTransform->inverted(&invertible);
	if (!invertible) {
		return std::nullopt;
	}
	return Ui::DamageRect(inverted.map(
		p.transform().map(QPolygonF(rect))
	).boundingRect());
}

QSize Poll::countOptimalSize() {
	updateTexts();

	const auto paddings = st::msgPadding.left() + st::msgPadding.right();

	auto maxWidth = st::msgFileMinWidth;
	accumulate_max(maxWidth, paddings + _headerPart->_description.maxWidth());
	accumulate_max(maxWidth, paddings + _headerPart->_question.maxWidth());
	for (const auto &answer : _optionsPart->_answers) {
		const auto media = answer.thumbnail
			? (PollAnswerMediaSize() + PollAnswerMediaSkip())
			: 0;
		accumulate_max(
			maxWidth,
			paddings
			+ st::historyPollAnswerPadding.left()
			+ answer.text.maxWidth()
			+ media
			+ st::historyPollAnswerPadding.right());
	}

	const auto innerWidth = maxWidth - paddings;
	auto minHeight = _headerPart->countHeight(innerWidth)
		+ _optionsPart->countHeight(innerWidth)
		+ _addOptionPart->countHeight(innerWidth)
		+ _footerPart->countHeight(innerWidth);
	return { maxWidth, minHeight };
}

bool Poll::showVotes() const {
	if (_adminShowResults) {
		return true;
	}
	if (_flags & PollData::Flag::HideResultsUntilClose) {
		return (_flags & PollData::Flag::Closed) || _poll->creator();
	}
	return _voted
		|| (_flags & PollData::Flag::Closed)
		|| voteRestricted();
}

PollData::VoteRestriction Poll::knownVoteRestriction() const {
	const auto fromServer = _poll->voteRestriction();
	if (fromServer != PollData::VoteRestriction::None) {
		if (IsExpiringVoteRestriction(fromServer)) {
			const auto updated = _poll->voteRestrictionUpdated();
			if (updated > 0
				&& (updated + kExpiringVoteRestrictionDuration <= crl::now())) {
				return PollData::VoteRestriction::None;
			}
		}
		return fromServer;
	}
	if (_poll->subscribersOnly()) {
		const auto channel = _parent->data()->history()->peer->asChannel();
		if (channel && !channel->amIn()) {
			return PollData::VoteRestriction::SubscribersOnly;
		}
	}
	if (!_poll->countries.empty()) {
		const auto userIso2 = _poll->session().appConfig().phoneCountryIso2();
		if (!userIso2.isEmpty()) {
			const auto inList = ranges::any_of(
				_poll->countries,
				[&](const QString &iso2) {
					return !iso2.compare(userIso2, Qt::CaseInsensitive);
				});
			if (!inList) {
				return PollData::VoteRestriction::Countries;
			}
		}
	}
	return PollData::VoteRestriction::None;
}

bool Poll::voteRestricted() const {
	if (knownVoteRestriction() == PollData::VoteRestriction::None) {
		return false;
	} else if (_voted
		|| _adminShowResults
		|| !_parent->data()->isRegular()) {
		return false;
	} else if (_flags & PollData::Flag::HideResultsUntilClose) {
		return !(_flags & PollData::Flag::Closed) && !_poll->creator();
	}
	return !(_flags & PollData::Flag::Closed);
}

void Poll::showVoteRestrictionToast() const {
	const auto restriction = knownVoteRestriction();
	if (restriction == PollData::VoteRestriction::None) {
		return;
	}
	const auto peer = _parent->data()->history()->peer;
	auto text = PollVoteRestrictionText(restriction, peer, _poll);
	if (text.text.isEmpty()) {
		return;
	}
	if (const auto window = peer->session().tryResolveWindow(peer)) {
		window->showToast({
			.text = std::move(text),
			.iconLottie = u"ban"_q,
			.iconLottieSize = st::pollToastIconSize,
			.duration = kVoteRestrictionToastDuration,
		});
	}
}

bool Poll::isAuthorNotVoted() const {
	return _parent->data()->out()
		&& !_voted
		&& !(_flags & PollData::Flag::Closed);
}

bool Poll::canVote() const {
	return !showVotes()
		&& !_voted
		&& _parent->data()->isRegular()
		&& (knownVoteRestriction() == PollData::VoteRestriction::None);
}

bool Poll::canSendVotes() const {
	return canVote() && _optionsPart->_hasSelected;
}

bool Poll::showVotersCount() const {
	if (voteRestricted()) {
		return true;
	}
	if (_voted && !showVotes()) {
		return !(_flags & PollData::Flag::PublicVotes);
	}
	return showVotes()
		? (!_totalVotes || !(_flags & PollData::Flag::PublicVotes))
		: !(_flags & PollData::Flag::MultiChoice);
}

bool Poll::inlineFooter() const {
	return !(_flags
		& (PollData::Flag::PublicVotes | PollData::Flag::MultiChoice));
}

bool Poll::canAddOption() const {
	return (_flags & PollData::Flag::OpenAnswers)
		&& !(_flags & PollData::Flag::Closed)
		&& !_parent->data()->Has<HistoryMessageForwarded>()
		&& (int(_poll->answers.size())
			< _poll->session().appConfig().pollOptionsLimit());
}

QRect Poll::addOptionRect(int innerWidth) const {
	const auto answersHeight = ranges::accumulate(ranges::views::all(
		_optionsPart->_answers
	) | ranges::views::transform([&](const Answer &answer) {
		return _optionsPart->countAnswerHeight(answer, innerWidth);
	}), 0);
	const auto top = _headerPart->countQuestionTop(innerWidth)
		+ _headerPart->_question.countHeight(innerWidth)
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip
		+ answersHeight
		+ (st::historyPollChoiceRight.height()
			- st::historyPollFillingHeight) / 2;
	return QRect(
		st::msgPadding.left(),
		top,
		innerWidth,
		_addOptionPart->countHeight(innerWidth));
}

void Poll::setAddOptionActive(bool active) {
	if (_addOptionActive != active) {
		_addOptionActive = active;
		history()->owner().requestViewResize(_parent);
	}
}

int Poll::Options::countAnswerContentWidth(
		const Answer &answer,
		int innerWidth) const {
	const auto answerWidth = innerWidth
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();
	const auto mediaWidth = answer.thumbnail
		? (PollAnswerMediaSize() + PollAnswerMediaSkip())
		: 0;
	return std::max(1, answerWidth - mediaWidth);
}

int Poll::Options::countVotesExtraHeight(
		const Answer &answer,
		int textWidth) const {
	if (answer.votesCountString.isEmpty()
		&& answer.recentVoters.empty()) {
		return 0;
	}
	const auto lineWidths = answer.text.countLineWidths(textWidth);
	if (lineWidths.empty()) {
		return 0;
	}
	const auto voterCount = int(answer.recentVoters.size());
	const auto &ust = st::historyPollAnswerUserpics;
	const auto userpicsWidth = voterCount
		? (ust.size + (voterCount - 1) * (ust.size - ust.shift))
		: 0;
	const auto userpicsExtra = userpicsWidth
		? (st::historyPollAnswerUserpicsSkip + userpicsWidth)
		: 0;
	const auto votesWidth = answer.votesCountWidth
		+ userpicsExtra
		+ st::historyPollFillingRight
		+ st::historyPollPercentSkip;
	const auto lastLineWidth = lineWidths.back();
	if (lastLineWidth + votesWidth <= textWidth) {
		return 0;
	}
	return st::normalFont->height;
}

int Poll::Options::countAnswerHeight(
		const Answer &answer,
		int innerWidth) const {
	const auto media = answer.thumbnail ? PollAnswerMediaSize() : 0;
	const auto textWidth = countAnswerContentWidth(answer, innerWidth);
	const auto &padding = answer.thumbnail
		? st::historyPollAnswerPadding
		: st::historyPollAnswerPaddingNoMedia;
	const auto textHeight = answer.text.countHeight(textWidth);
	const auto multiline = (textHeight
		> st::historyPollPercentFont->height);
	const auto votesExtra = _owner->showVotes()
		? countVotesExtraHeight(answer, textWidth)
		: 0;
	const auto fillingWithChoice = (_owner->showVotes()
		&& (media || multiline || votesExtra))
		? (std::max(textHeight, st::historyPollPercentFont->height)
			+ votesExtra
			+ st::historyPollFillingTop
			+ (st::historyPollFillingHeight
				+ st::historyPollChoiceRight.height()) / 2)
		: 0;
	return padding.top()
		+ std::max({
			textHeight,
			media,
			fillingWithChoice,
		})
		+ padding.bottom();
}

QSize Poll::countCurrentSize(int newWidth) {
	_headerPart->invalidateTextRepaints();
	_optionsPart->invalidateAnswerTextRepaints();
	_optionsPart->invalidateAnimatedThumbnailRepaints();
	invalidateFiniteRepaintGeometries();
	accumulate_min(newWidth, maxWidth());
	const auto innerWidth = newWidth
		- st::msgPadding.left()
		- st::msgPadding.right();

	auto newHeight = _headerPart->countHeight(innerWidth)
		+ _optionsPart->countHeight(innerWidth)
		+ _addOptionPart->countHeight(innerWidth)
		+ _footerPart->countHeight(innerWidth);
	return { newWidth, newHeight };
}

void Poll::updateTexts() {
	const auto current = _poll->owner().poll(_poll->id);
	if (_poll != current) {
		_poll = current;
		_pollVersion = 0;
	}
	if (_pollVersion == _poll->version) {
		return;
	}
	const auto first = !_pollVersion;
	_pollVersion = _poll->version;
	invalidateFiniteRepaintGeometries();

	const auto willStartAnimation = _optionsPart->checkAnimationStart();
	const auto voted = _voted;

	_headerPart->updateDescription();
	if (_headerPart->_question.toTextWithEntities() != _poll->question) {
		auto options = Ui::WebpageTextTitleOptions();
		options.maxw = options.maxh = 0;
		_headerPart->_question.setMarkedText(
			st::historyPollQuestionStyle,
			_poll->question,
			options,
			Core::TextContext({
				.session = &_poll->session(),
				.repaint = _headerPart->textRepaintCallback(
					Header::TextPart::Question),
				.customEmojiLoopLimit = 2,
			}));
	}
	if (_flags != _poll->flags() || _headerPart->_subtitle.isEmpty()) {
		using Flag = PollData::Flag;
		_flags = _poll->flags();
		_headerPart->_subtitle.setText(
			st::msgDateTextStyle,
			((_flags & Flag::Closed)
				? tr::lng_polls_closed(tr::now)
				: (_flags & Flag::Quiz)
				? ((_flags & Flag::PublicVotes)
					? tr::lng_polls_public_quiz(tr::now)
					: tr::lng_polls_anonymous_quiz(tr::now))
				: ((_flags & Flag::PublicVotes)
					? tr::lng_polls_public(tr::now)
					: tr::lng_polls_anonymous(tr::now))));
	}
	if (_footerPart->_adminBackVoteLabel.isEmpty()) {
		_footerPart->_adminBackVoteLabel.setMarkedText(
			st::semiboldTextStyle,
			tr::lng_polls_admin_back_vote(
				tr::now,
				lt_arrow,
				Ui::Text::IconEmoji(&st::textBackIconEmoji),
				tr::marked));
	}
	_headerPart->updateRecentVoters();
	_optionsPart->updateAnswers();
	_headerPart->updateAttachedMedia();
	_headerPart->updateSolutionText();
	_headerPart->updateSolutionMedia();
	refreshWebpageSubscriptions();
	updateVotes();

	if (willStartAnimation) {
		_optionsPart->startAnswersAnimation();
		if (!voted) {
			_optionsPart->checkQuizAnswered();
		}
	}
	_headerPart->solutionToggled(
		_headerPart->_solutionShown,
		first ? anim::type::instant : anim::type::normal);
}

Fn<void()> Poll::Header::textRepaintCallback(TextPart part) {
	auto &repaint = textRepaint(part);
	invalidateTextRepaint(repaint);
	const auto generation = ++repaint.generation;
	return crl::guard(_owner, [=] {
		_owner->_headerPart->repaintText(part, generation);
	});
}

void Poll::Header::resetTextRepaint(TextPart part) {
	auto &repaint = textRepaint(part);
	invalidateTextRepaint(repaint);
	++repaint.generation;
}

void Poll::Header::repaintText(
		TextPart part,
		uint64 generation) const {
	auto &repaint = textRepaint(part);
	if (generation != repaint.generation
		|| repaint.pending
		|| _owner->_parent->delegate()->elementAnimationsPaused()) {
		return;
	} else if (repaint.known && repaint.current.isEmpty()) {
		return;
	}
	repaint.pending = true;
	if (!repaint.known) {
		_owner->repaint();
	} else {
		repaintTextRegion(repaint.current);
	}
}

void Poll::Header::finishTextRepaint(
		TextPart part,
		const Painter &p,
		const PaintContext &context,
		QRegion region,
		bool geometryKnown) const {
	if (!context.hasElementPainter(p)) {
		return;
	}
	auto &repaint = textRepaint(part);
	repaint.pending = false;
	if (!geometryKnown) {
		region = QRegion();
	}
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.current = std::move(region);
	repaint.known = geometryKnown;
	if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = true;
	repaintTextRegion(previous.united(repaint.current));
}

void Poll::Header::invalidateTextRepaints() const {
	invalidateTextRepaint(_descriptionRepaint);
	invalidateTextRepaint(_questionRepaint);
	invalidateTextRepaint(_solutionRepaint);
}

void Poll::Header::invalidateTextRepaint(TextRepaint &repaint) const {
	repaint.stale = repaint.stale.united(base::take(repaint.current));
	repaint.pending = false;
	repaint.known = false;
}

void Poll::Header::repaintTextRegion(const QRegion &region) const {
	_owner->_parent->repaint(region);
}

Poll::Header::TextRepaint &Poll::Header::textRepaint(
		TextPart part) const {
	switch (part) {
	case TextPart::Description: return _descriptionRepaint;
	case TextPart::Question: return _questionRepaint;
	case TextPart::Solution: return _solutionRepaint;
	}
	Unexpected("Text part in Poll::Header::textRepaint.");
}

void Poll::Header::updateDescription() {
	const auto media = _owner->_parent->data()->media();
	const auto consumed = media
		? media->consumedMessageText()
		: TextWithEntities();
	if (consumed.text.isEmpty()) {
		if (!_description.isEmpty()) {
			resetTextRepaint(TextPart::Description);
			_description = Ui::Text::String(st::msgMinWidth / 2);
		}
		return;
	}
	if (_description.toTextWithEntities() == consumed) {
		return;
	}
	const auto context = Core::TextContext({
		.session = &_owner->_poll->session(),
		.repaint = textRepaintCallback(TextPart::Description),
		.customEmojiLoopLimit = 2,
	});
	_description.setMarkedText(
		st::webPageDescriptionStyle,
		consumed,
		Ui::ItemTextOptions(_owner->_parent->data()),
		context);
	InitElementTextPart(_owner->_parent, _description);
}

void Poll::Header::updateSolutionText() {
	if (_owner->_poll->solution.text.isEmpty()) {
		if (!_solutionText.isEmpty()) {
			resetTextRepaint(TextPart::Solution);
			_solutionText = Ui::Text::String();
		}
		return;
	}
	if (_solutionText.toTextWithEntities() == _owner->_poll->solution) {
		return;
	}
	auto repaint = textRepaintCallback(TextPart::Solution);
	_solutionText = Ui::Text::String(st::msgMinWidth);
	_solutionText.setMarkedText(
		st::webPageDescriptionStyle,
		_owner->_poll->solution,
		Ui::ItemTextOptions(_owner->_parent->data()),
		Core::TextContext({
			.session = &_owner->_poll->session(),
			.repaint = std::move(repaint),
		}));
	InitElementTextPart(_owner->_parent, _solutionText);
}

void Poll::Header::updateSolutionMedia() {
	const auto item = _owner->_parent->data();
	const auto messageContext = Window::SessionController::MessageContext{
		.id = item->fullId(),
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = item->sublistPeerId(),
	};
	const auto paused = [=] {
		return _owner->_parent->delegate()->elementAnimationsPaused();
	};
	const auto updated = MakePollThumbnail(
		_owner->_poll,
		_owner->_poll->solutionMedia,
		messageContext,
		paused);
	if (!updated.thumbnail) {
		_solutionMedia = nullptr;
		_solutionAttach = nullptr;
		return;
	}
	if (_solutionMedia
		&& _solutionMedia->kind == updated.kind
		&& _solutionMedia->id == updated.id) {
		return;
	}
	if (!_solutionMedia) {
		_solutionMedia = std::make_unique<SolutionMedia>();
	}
	_solutionMedia->kind = updated.kind;
	_solutionMedia->id = updated.id;
	auto photo = (PhotoData*)(nullptr);
	auto document = (DocumentData*)(nullptr);
	if (updated.kind == PollThumbnailKind::Photo && updated.id) {
		photo = _owner->_poll->owner().photo(PhotoId(updated.id));
	} else if (updated.kind == PollThumbnailKind::Document && updated.id) {
		document = _owner->_poll->owner().document(DocumentId(updated.id));
	} else if (updated.kind == PollThumbnailKind::Audio && updated.id) {
		document = _owner->_poll->owner().document(DocumentId(updated.id));
	} else if (updated.kind == PollThumbnailKind::Geo
		&& _owner->_poll->solutionMedia.geo) {
		const auto &point = *_owner->_poll->solutionMedia.geo;
		const auto cloudImage = _owner->_poll->owner().location(point);
		_solutionAttach = std::make_unique<Location>(
			_owner->_parent,
			cloudImage,
			point,
			QString(),
			QString());
		return;
	}
	_solutionAttach = (photo || document)
		? CreateAttach(_owner->_parent, document, photo)
		: nullptr;
}

void Poll::Header::updateAttachedMedia() {
	const auto item = _owner->_parent->data();
	const auto messageContext = Window::SessionController::MessageContext{
		.id = item->fullId(),
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = item->sublistPeerId(),
	};
	const auto paused = [=] {
		return _owner->_parent->delegate()->elementAnimationsPaused();
	};
	const auto updated = MakePollThumbnail(
		_owner->_poll,
		_owner->_poll->attachedMedia,
		messageContext,
		paused);
	const auto same = (_attachedMedia->kind == updated.kind)
		&& (_attachedMedia->id == updated.id)
		&& (_attachedMedia->rounded == updated.rounded);
	if (same) {
		return;
	}
	if (_attachedMedia->thumbnail) {
		_attachedMedia->thumbnail->subscribeToUpdates(nullptr);
	}
	_attachedMediaCache = QImage();
	_attachedMedia->thumbnail = updated.thumbnail;
	_attachedMedia->handler = updated.handler;
	_attachedMedia->kind = updated.kind;
	_attachedMedia->rounded = updated.rounded;
	_attachedMedia->id = updated.id;
	_attachedMedia->photo = nullptr;
	_attachedMedia->photoMedia = nullptr;
	_attachedMedia->photoSize = QSize();
	if (updated.kind == PollThumbnailKind::Photo && updated.id) {
		const auto photo = _owner->_poll->owner().photo(PhotoId(updated.id));
		_attachedMedia->photo = photo;
		_attachedMedia->photoMedia = photo->createMediaView();
		_attachedMedia->photoMedia->wanted(
			Data::PhotoSize::Large,
			_owner->_parent->data()->fullId());
		if (const auto size = photo->size(Data::PhotoSize::Large)) {
			_attachedMedia->photoSize = *size;
		} else if (const auto s = photo->size(Data::PhotoSize::Thumbnail)) {
			_attachedMedia->photoSize = *s;
		}
	}
	if ((updated.kind == PollThumbnailKind::Document
		|| updated.kind == PollThumbnailKind::Audio) && updated.id) {
		const auto document = _owner->_poll->owner().document(
			DocumentId(updated.id));
		_attachedMediaAttach = CreateAttach(
			_owner->_parent,
			document,
			nullptr);
	} else {
		_attachedMediaAttach = nullptr;
	}
	if (_attachedMedia->thumbnail) {
		_attachedMedia->thumbnail->subscribeToUpdates(
			crl::guard(_owner, [=] {
				if (!_owner->_parent->delegate()->elementAnimationsPaused()) {
					_attachedMediaCache = QImage();
					_owner->repaint();
				}
			}));
	}
}

int Poll::Header::countTopContentSkip(int pollWidth) const {
	return countTopMediaHeight(pollWidth)
		? st::historyPollMediaTopSkip
		: _owner->isBubbleTop()
		? st::historyPollQuestionTop
		: (st::historyPollQuestionTop - st::msgFileTopMinus);
}

int Poll::Header::countTopMediaHeight(int pollWidth) const {
	if (!_attachedMedia || !_attachedMedia->thumbnail) {
		return 0;
	}
	if (_attachedMediaAttach) {
		return countAttachHeight(pollWidth);
	}
	if (_attachedMedia->kind == PollThumbnailKind::Photo
		&& !_attachedMedia->photoSize.isEmpty()) {
		const auto w = pollWidth > 0 ? pollWidth : _owner->width();
		const auto sideSkip = st::historyPollMediaSideSkip;
		const auto availableWidth = w - 2 * sideSkip;
		const auto &original = _attachedMedia->photoSize;
		return std::max(
			1,
			int(original.height() * availableWidth / original.width()));
	}
	return st::historyPollMediaHeight;
}

int Poll::Header::countAttachHeight(int pollWidth) const {
	if (!_attachedMediaAttach) {
		return 0;
	}
	_attachedMediaAttach->initDimensions();
	const auto w = pollWidth > 0 ? pollWidth : _owner->width();
	const auto sideSkip = st::historyPollMediaSideSkip;
	const auto innerWidth = w - 2 * sideSkip;
	return _attachedMediaAttach->resizeGetHeight(
		std::max(1, innerWidth));
}

QRect Poll::Header::countTopMediaRect(int top) const {
	const auto sideSkip = st::historyPollMediaSideSkip;
	const auto mediaHeight = countTopMediaHeight();
	return mediaHeight
		? QRect(
			sideSkip,
			top,
			std::max(1, _owner->width() - 2 * sideSkip),
			mediaHeight)
		: QRect();
}

Ui::BubbleRounding Poll::Header::topMediaRounding() const {
	using Corner = Ui::BubbleCornerRounding;
	auto result = _owner->adjustedBubbleRounding(
		RectPart::BottomLeft | RectPart::BottomRight);
	const auto normalize = [](Corner value) {
		return (value == Corner::Large)
			? Corner::Large
			: (value == Corner::None)
			? Corner::None
			: Corner::Small;
	};
	result.topLeft = normalize(result.topLeft);
	result.topRight = normalize(result.topRight);
	result.bottomLeft = Corner::Small;
	result.bottomRight = Corner::Small;
	return result;
}

void Poll::Header::validateTopMediaCache(QSize size) const {
	if (!_attachedMedia || !_attachedMedia->thumbnail || size.isEmpty()) {
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	const auto rounding = topMediaRounding();
	auto source = QImage();
	auto blurred = false;
	if (_attachedMedia->photoMedia) {
		if (const auto image
			= _attachedMedia->photoMedia->image(Data::PhotoSize::Large)) {
			source = image->original();
		} else if (const auto image
			= _attachedMedia->photoMedia->image(
				Data::PhotoSize::Thumbnail)) {
			source = image->original();
			blurred = true;
		} else if (const auto image
			= _attachedMedia->photoMedia->thumbnailInline()) {
			source = image->original();
			blurred = true;
		}
	}
	if (source.isNull()) {
		source = _attachedMedia->thumbnail->image(
			std::max(size.width(), size.height()) * ratio);
	}
	if (source.isNull()) {
		return;
	}
	if ((_attachedMediaCache.size() == (size * ratio))
		&& (_attachedMediaCacheRounding == rounding)
		&& (_attachedMediaCacheBlurred == blurred)) {
		return;
	}
	const auto sw = source.width();
	const auto sh = source.height();
	const auto tw = size.width() * ratio;
	const auto th = size.height() * ratio;
	if (sw * th != sh * tw) {
		const auto cropW = std::min(sw, sh * tw / th);
		const auto cropH = std::min(sh, sw * th / tw);
		source = source.copy(
			(sw - cropW) / 2,
			(sh - cropH) / 2,
			cropW,
			cropH);
	}
	const auto options = blurred
		? Images::Option::Blur
		: Images::Option();
	auto prepared = Images::Prepare(
		source,
		size * ratio,
		{ .options = options, .outer = size });
	prepared = Images::Round(
		std::move(prepared),
		MediaRoundingMask(rounding));
	prepared.setDevicePixelRatio(ratio);
	_attachedMediaCache = std::move(prepared);
	_attachedMediaCacheRounding = rounding;
	_attachedMediaCacheBlurred = blurred;
}

int Poll::Header::countDescriptionHeight(int innerWidth) const {
	return _description.isEmpty() ? 0 : _description.countHeight(innerWidth);
}

int Poll::Header::countSolutionMediaHeight(int mediaWidth) const {
	if (!_solutionAttach) {
		return 0;
	}
	_solutionAttach->initDimensions();
	return _solutionAttach->resizeGetHeight(mediaWidth);
}

int Poll::Header::countSolutionBlockHeight(int innerWidth) const {
	if (!_solutionShown || !canShowSolution()) {
		return 0;
	}
	const auto &qst = st::historyPagePreview;
	const auto textWidth = innerWidth
		- qst.padding.left()
		- qst.padding.right();
	auto height = qst.padding.top();
	height += st::semiboldFont->height;
	height += st::historyPollExplanationTitleSkip;
	height += _solutionText.countHeight(textWidth);
	if (const auto mediaHeight = countSolutionMediaHeight(textWidth)) {
		height += st::historyPollExplanationMediaSkip + mediaHeight;
	}
	height += qst.padding.bottom();
	return height;
}

int Poll::Header::countQuestionTop(int innerWidth, int pollWidth) const {
	auto result = countTopContentSkip(pollWidth);
	if (const auto mediaHeight = countTopMediaHeight(pollWidth)) {
		result += mediaHeight + st::historyPollMediaSkip;
	}
	if (const auto descriptionHeight = countDescriptionHeight(innerWidth)) {
		result += descriptionHeight + st::historyPollDescriptionSkip;
	}
	if (const auto solutionHeight = countSolutionBlockHeight(innerWidth)) {
		result += solutionHeight + st::historyPollExplanationSkip;
	}
	return result;
}

uint16 Poll::Header::solutionSelectionLength() const {
	return (_solutionShown && canShowSolution())
		? _solutionText.length()
		: uint16(0);
}

TextSelection Poll::Header::toSolutionSelection(
		TextSelection selection) const {
	return UnshiftItemSelection(selection, _description);
}

TextSelection Poll::Header::fromSolutionSelection(
		TextSelection selection) const {
	return ShiftItemSelection(selection, _description);
}

TextSelection Poll::Header::toQuestionSelection(
		TextSelection selection) const {
	return UnshiftItemSelection(
		selection,
		uint16(_description.length() + solutionSelectionLength()));
}

TextSelection Poll::Header::fromQuestionSelection(
		TextSelection selection) const {
	return ShiftItemSelection(
		selection,
		uint16(_description.length() + solutionSelectionLength()));
}

void Poll::Options::checkQuizAnswered() {
	if (!_owner->_voted
		|| !_votedFromHere
		|| !_owner->_poll->quiz()
		|| anim::Disabled()) {
		return;
	}
	const auto i = ranges::find(_answers, true, &Answer::chosen);
	if (i == end(_answers)) {
		return;
	}
	if (i->correct) {
		const auto generation = _owner->startRepaintGeneration(
			_owner->_fireworksRepaint);
		const auto weak = base::make_weak(_owner.get());
		_owner->_fireworksAnimation = std::make_unique<Ui::FireworksAnimation>(
			[=] {
				if (const auto strong = weak.get()) {
					strong->repaintGeometry(
						strong->_fireworksRepaint,
						generation);
				}
			});
	} else {
		const auto generation = _owner->startRepaintGeneration(
			_owner->_wrongAnswerRepaint);
		const auto weak = base::make_weak(_owner.get());
		_owner->_wrongAnswerAnimation.start(
			[=] {
				if (const auto strong = weak.get()) {
					strong->repaintGeometry(
						strong->_wrongAnswerRepaint,
						generation);
				}
			},
			0.,
			1.,
			kRollDuration,
			anim::linear);
		_owner->_headerPart->showSolution();
	}
}

void Poll::Header::showSolution() const {
	if (!_owner->_poll->solution.text.isEmpty()) {
		solutionToggled(true);
	}
}

void Poll::Header::solutionToggled(
		bool solutionShown,
		anim::type animated) const {
	_solutionShown = solutionShown;
	const auto visible = canShowSolution() && !_solutionShown;
	if (_solutionButtonVisible == visible) {
		if (animated == anim::type::instant
			&& _solutionButtonAnimation.animating()) {
			_solutionButtonAnimation.stop();
			_owner->stopRepaintGeneration(_solutionButtonRepaint);
			_owner->repaintGeometry(_solutionButtonRepaint);
		}
		return;
	}
	_solutionButtonVisible = visible;
	if (animated == anim::type::instant) {
		_solutionButtonAnimation.stop();
		_owner->stopRepaintGeneration(_solutionButtonRepaint);
		_owner->repaintGeometry(_solutionButtonRepaint);
	} else {
		const auto generation = _owner->startRepaintGeneration(
			_solutionButtonRepaint);
		const auto weak = base::make_weak(_owner.get());
		_solutionButtonAnimation.start(
			[=] {
				if (const auto strong = weak.get()) {
					strong->repaintGeometry(
						strong->_headerPart->_solutionButtonRepaint,
						generation);
				}
			},
			visible ? 0. : 1.,
			visible ? 1. : 0.,
			st::fadeWrapDuration);
	}
	_owner->history()->owner().requestViewResize(_owner->_parent);
}

void Poll::Header::updateRecentVoters() {
	auto &&sliced = ranges::views::all(
		_owner->_poll->recentVoters
	) | ranges::views::take(kShowRecentVotersCount);
	const auto changed = !ranges::equal(
		_recentVoters,
		sliced,
		ranges::equal_to(),
		&RecentVoter::peer);
	if (changed) {
		auto updated = ranges::views::all(
			sliced
		) | ranges::views::transform([](not_null<PeerData*> peer) {
			return RecentVoter{ peer };
		}) | ranges::to_vector;
		const auto has = _owner->hasHeavyPart();
		if (has) {
			for (auto &voter : updated) {
				const auto i = ranges::find(
					_recentVoters,
					voter.peer,
					&RecentVoter::peer);
				if (i != end(_recentVoters)) {
					voter.userpic = std::move(i->userpic);
				}
			}
		}
		_recentVoters = std::move(updated);
		if (has && !_owner->hasHeavyPart()) {
			_owner->_parent->checkHeavyPart();
		}
	}
}

void Poll::Options::updateAnswers() {
	invalidateAnswerTextRepaints();
	invalidateAnimatedThumbnailRepaints();
	invalidateFeedbackRepaints();
	const auto paused = [=] {
		return _owner->_parent->delegate()->elementAnimationsPaused();
	};
	const auto item = _owner->_parent->data();
	const auto messageContext = Window::SessionController::MessageContext{
		.id = item->fullId(),
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = item->sublistPeerId(),
	};
	auto options = ranges::views::all(
		_owner->_poll->answers
	) | ranges::views::transform(&PollAnswer::option) | ranges::to_vector;
	if ((_owner->_flags & PollData::Flag::ShuffleAnswers)
		&& !(_owner->_flags & PollData::Flag::Creator)) {
		const auto userId = _owner->_poll->session().userId();
		const auto pollId = _owner->_poll->id;
		ranges::sort(options, [&](const QByteArray &a, const QByteArray &b) {
			const auto hashA = HashPollShuffleValue(userId, pollId, a);
			const auto hashB = HashPollShuffleValue(userId, pollId, b);
			return (hashA == hashB) ? (a < b) : (hashA < hashB);
		});
	}
	const auto changed = (_answers.size() != options.size())
		|| !ranges::equal(
			_answers,
			options,
			ranges::equal_to(),
			&Answer::option);
	if (!changed) {
		for (auto &answer : _answers) {
			const auto i = ranges::find(
				_owner->_poll->answers,
				answer.option,
				&PollAnswer::option);
			Assert(i != end(_owner->_poll->answers));
			fillAnswerData(answer, *i);
			answer.fillMedia(
				_owner->_poll,
				*i,
				messageContext,
				this,
				paused);
		}
		_anyAnswerHasMedia = ranges::any_of(_answers, [](const Answer &a) {
			return a.thumbnail != nullptr;
		});
		syncAnswerTextRepaints();
		syncAnimatedThumbnailRepaints();
		syncFeedbackRepaints();
		return;
	}
	_answers = ranges::views::all(options) | ranges::views::transform([&](
			const QByteArray &option) {
		auto result = Answer();
		result.option = option;
		const auto i = ranges::find(
			_owner->_poll->answers,
			option,
			&PollAnswer::option);
		Assert(i != end(_owner->_poll->answers));
		fillAnswerData(result, *i);
		result.fillMedia(
			_owner->_poll,
			*i,
			messageContext,
			this,
			paused);
		return result;
	}) | ranges::to_vector;

	if ((_owner->_flags & PollData::Flag::ShuffleAnswers)
		&& !(_owner->_flags & PollData::Flag::Creator)) {
		const auto visitorId = _owner->_poll->session().userId();
		const auto pollId = _owner->_poll->id;
		ranges::sort(_answers, [&](const Answer &a, const Answer &b) {
			return HashPollShuffleValue(visitorId, pollId, a.option)
				< HashPollShuffleValue(visitorId, pollId, b.option);
		});
	}

	for (auto &answer : _answers) {
		answer.handler = createAnswerClickHandler(answer);
	}
	_anyAnswerHasMedia = ranges::any_of(_answers, [](const Answer &a) {
		return a.thumbnail != nullptr;
	});
	syncAnswerTextRepaints();
	syncAnimatedThumbnailRepaints();
	syncFeedbackRepaints();

	resetAnswersAnimation();
}

ClickHandlerPtr Poll::Options::createAnswerClickHandler(
		const Answer &answer) {
	const auto option = answer.option;
	auto result = ClickHandlerPtr();
	if (_owner->_flags & PollData::Flag::MultiChoice) {
		result = std::make_shared<LambdaClickHandler>(crl::guard(_owner, [=] {
			if (Logs::DebugEnabled() && base::IsCtrlPressed()) {
				TextUtilities::SetClipboardText(
					TextForMimeData::Simple(_owner->_poll->debugString()));
				return;
			}
			if (_owner->canVote()) {
				_owner->_optionsPart->toggleMultiOption(option);
			} else if (_owner->voteRestricted()) {
				_owner->showVoteRestrictionToast();
			} else if (_owner->showVotes()) {
				_owner->_optionsPart->showAnswerVotesTooltip(option);
			}
		}));
	} else {
		result = std::make_shared<LambdaClickHandler>(crl::guard(_owner, [=] {
			if (Logs::DebugEnabled() && base::IsCtrlPressed()) {
				TextUtilities::SetClipboardText(
					TextForMimeData::Simple(_owner->_poll->debugString()));
				return;
			}
			if (_owner->canVote()) {
				_owner->_optionsPart->_votedFromHere = true;
				_owner->history()->session().api().polls().sendVotes(
					_owner->_parent->data()->fullId(),
					{ option });
			} else if (_owner->voteRestricted()) {
				_owner->showVoteRestrictionToast();
			} else if (_owner->showVotes()) {
				_owner->_optionsPart->showAnswerVotesTooltip(option);
			}
		}));
	}
	result->setProperty(kPollOptionProperty, option);
	return result;
}

void Poll::Options::toggleMultiOption(const QByteArray &option) {
	const auto i = ranges::find(
		_answers,
		option,
		&Answer::option);
	if (i != end(_answers)) {
		const auto selected = i->selected;
		i->selected = !selected;
		const auto generation = resetFeedbackRepaint(
			option,
			FeedbackPart::Toggle);
		const auto weak = base::make_weak(_owner.get());
		i->selectedAnimation.start(
			[=] {
				if (const auto strong = weak.get()) {
					strong->_optionsPart->repaintAnswerFeedback(
						option,
						FeedbackPart::Toggle,
						generation);
				}
			},
			selected ? 1. : 0.,
			selected ? 0. : 1.,
			st::defaultCheck.duration);
		if (selected) {
			const auto j = ranges::find(
				_answers,
				true,
				&Answer::selected);
			_hasSelected = (j != end(_answers));
		} else {
			_hasSelected = true;
		}
		repaintAnswerFeedback(option, FeedbackPart::Toggle);
		_owner->repaintGeometry(_owner->_footerPart->_contentRepaint);
	}
}

void Poll::Options::clearSelected() {
	auto changed = false;
	for (auto &answer : _answers) {
		const auto repaint = answer.selected
			|| answer.selectedAnimation.animating();
		if (answer.selected) {
			answer.selected = false;
			changed = true;
		}
		if (answer.selectedAnimation.animating()) {
			answer.selectedAnimation.stop();
		}
		if (repaint) {
			if (const auto repaints = findFeedbackRepaints(answer.option)) {
				_owner->stopRepaintGeneration(repaints->toggle);
			}
			repaintAnswerFeedback(
				answer.option,
				FeedbackPart::Toggle);
		}
	}
	if (_hasSelected) {
		_hasSelected = false;
		changed = true;
	}
	if (changed) {
		_owner->repaintGeometry(_owner->_footerPart->_contentRepaint);
	}
}

void Poll::Options::sendMultiOptions() {
	auto chosen = _answers | ranges::views::filter(
		&Answer::selected
	) | ranges::views::transform(
		&Answer::option
	) | ranges::to_vector;
	if (!chosen.empty()) {
		_votedFromHere = true;
		_owner->history()->session().api().polls().sendVotes(
			_owner->_parent->data()->fullId(),
			std::move(chosen));
	}
}

void Poll::Options::showAnswerVotesTooltip(const QByteArray &option) {
	const auto answer = _owner->_poll->answerByOption(option);
	if (!answer) {
		return;
	}
	const auto quiz = _owner->_poll->quiz();
	const auto text = answer->votes
		? (quiz
			? tr::lng_polls_answers_count
			: tr::lng_polls_votes_count)(
				tr::now,
				lt_count_decimal,
				answer->votes)
		: (quiz
			? tr::lng_polls_answers_none
			: tr::lng_polls_votes_none)(tr::now);
	_owner->_parent->delegate()->elementShowTooltip({ text }, [] {});
}

void Poll::Options::showResults() {
	_owner->_parent->delegate()->elementShowPollResults(
		_owner->_poll,
		_owner->_parent->data()->fullId());
}

void Poll::updateVotes() {
	const auto voted = _poll->voted();
	if (_voted != voted) {
		_voted = voted;
		if (_voted) {
			_optionsPart->clearSelected();
			if (_optionsPart->_votedFromHere
				&& (_flags & PollData::Flag::HideResultsUntilClose)
				&& !(_flags & PollData::Flag::Closed)) {
				Ui::Toast::Show({
					.text = tr::lng_polls_results_after_close(
						tr::now,
						tr::marked),
					.iconLottie = u"toast_hide_results"_q,
					.iconLottieSize = st::pollToastIconSize,
					.duration = crl::time(3000),
				});
			}
		} else {
			_optionsPart->_votedFromHere = false;
			_optionsPart->clearSelected();
		}
	}
	if (voteRestricted()) {
		_optionsPart->clearSelected();
	}
	_optionsPart->updateAnswerVotes();
	_footerPart->updateTotalVotes();
}

void Poll::Options::checkSendingAnimation() const {
	const auto &sending = _owner->_poll->sendingVotes;
	const auto sendingRadial = (sending.size() == 1)
		&& !(_owner->_flags & PollData::Flag::MultiChoice);
	if (sendingRadial == (_sendingAnimation != nullptr)) {
		if (_sendingAnimation) {
			_sendingAnimation->option = sending.front();
		}
		return;
	}
	if (!sendingRadial) {
		if (!_answersAnimation) {
			resetSendingAnimation();
		}
		return;
	}
	_sendingAnimation = std::make_unique<SendingAnimation>(
		sending.front(),
		[=] { radialAnimationCallback(); });
	_sendingAnimation->animation.start();
}

void Poll::Options::updateAnswerVotesFromOriginal(
		Answer &answer,
		const PollAnswer &original,
		int percent,
		int maxVotes,
		bool showPercent) {
	if (!_owner->showVotes() || !showPercent) {
		answer.votesPercent = 0;
		answer.votesPercentString.clear();
		answer.votesPercentWidth = 0;
	} else if (answer.votesPercentString.isEmpty()
		|| answer.votesPercent != percent) {
		answer.votesPercent = percent;
		answer.votesPercentString = QString::number(percent) + '%';
		answer.votesPercentWidth = st::historyPollPercentFont->width(
			answer.votesPercentString);
	}
	answer.chosen = original.chosen;
	answer.votes = original.votes;
	answer.filling = percent / 100.;
	if (_owner->showVotes() && answer.votes) {
		answer.votesCountString = Lang::FormatCountDecimal(answer.votes);
		answer.votesCountWidth = st::normalFont->width(
			answer.votesCountString);
	} else {
		answer.votesCountString.clear();
		answer.votesCountWidth = 0;
	}
	const auto changed = !ranges::equal(
		answer.recentVoters,
		original.recentVoters,
		ranges::equal_to(),
		&UserpicInRow::peer);
	if (changed) {
		auto updated = std::vector<UserpicInRow>();
		updated.reserve(original.recentVoters.size());
		for (const auto &peer : original.recentVoters) {
			const auto i = ranges::find(
				answer.recentVoters,
				peer,
				&UserpicInRow::peer);
			if (i != end(answer.recentVoters)) {
				updated.push_back(std::move(*i));
			} else {
				updated.push_back({ .peer = peer });
			}
		}
		answer.recentVoters = std::move(updated);
		answer.recentVotersImage = QImage();
	}
}

void Poll::Options::updateAnswerVotes() {
	if (_owner->_poll->answers.size() != _answers.size()
		|| _owner->_poll->answers.empty()) {
		return;
	}
	const auto totalVotes = _owner->_poll->totalVoters;
	const auto showPercent = (totalVotes > 0)
		&& ranges::all_of(_owner->_poll->answers, [=](const PollAnswer &a) {
			return a.votes <= totalVotes;
		});
	const auto maxVotes = std::max(1, ranges::max_element(
		_owner->_poll->answers,
		ranges::less(),
		&PollAnswer::votes)->votes);

	constexpr auto kMaxCount = PollData::kMaxOptions;
	const auto count = size_type(_owner->_poll->answers.size());
	Assert(count <= kMaxCount);
	int PercentsStorage[kMaxCount] = { 0 };
	int VotesStorage[kMaxCount] = { 0 };

	ranges::copy(
		ranges::views::all(
			_owner->_poll->answers
		) | ranges::views::transform(&PollAnswer::votes),
		ranges::begin(VotesStorage));

	if (showPercent) {
		CountNicePercent(
			gsl::make_span(VotesStorage).subspan(0, count),
			totalVotes,
			gsl::make_span(PercentsStorage).subspan(0, count));
	}

	for (auto &answer : _answers) {
		const auto i = ranges::find(
			_owner->_poll->answers,
			answer.option,
			&PollAnswer::option);
		Assert(i != end(_owner->_poll->answers));
		const auto index = int(i - begin(_owner->_poll->answers));
		updateAnswerVotesFromOriginal(
			answer,
			*i,
			PercentsStorage[index],
			maxVotes,
			showPercent);
	}
}

void Poll::draw(Painter &p, const PaintContext &context) const {
	rememberElementPaint(p, context);
	if (_repaintLayoutSize != currentSize()) {
		invalidateFiniteRepaintGeometries();
		_repaintLayoutSize = currentSize();
	}
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		if (context.hasElementPainter(p)) {
			recordRepaintGeometry(
				_headerPart->_solutionButtonRepaint,
				QRegion(),
				true);
			recordRepaintGeometry(
				_addOptionPart->_rippleRepaint,
				QRegion(),
				true);
			recordRepaintGeometry(
				_footerPart->_contentRepaint,
				QRegion(),
				true);
			recordRepaintGeometry(
				_footerPart->_rippleRepaint,
				QRegion(),
				true);
			for (auto &repaints : _optionsPart->_feedbackRepaints) {
				recordRepaintGeometry(repaints.toggle, QRegion(), true);
				recordRepaintGeometry(repaints.ripple, QRegion(), true);
			}
			_optionsPart->finishFeedbackRepaints();
		}
		return;
	}
	auto paintw = width();

	if (_poll->checkResultsReload(context.now)) {
		history()->session().api().polls().reloadResults(_parent->data());
	}

	const auto padding = st::msgPadding;
	paintw -= padding.left() + padding.right();

	const Part *parts[] = {
		_headerPart.get(),
		_optionsPart.get(),
		_addOptionPart.get(),
		_footerPart.get(),
	};
	auto tshift = 0;
	for (const auto part : parts) {
		const auto h = part->countHeight(paintw);
		const auto saved = p.transform();
		p.translate(0, tshift);
		part->draw(p, padding.left(), paintw, width(), context);
		p.setTransform(saved);
		tshift += h;
	}
}

void Poll::Options::resetAnswersAnimation() const {
	if (_answersAnimation) {
		const auto animation = base::take(_answersAnimation);
		const auto repaintRegion = animation->repaintRegion.united(
			animation->collectedRepaintRegion);
		repaintAnswersAnimationRegion(repaintRegion);
	}
	if (_owner->_poll->sendingVotes.size() != 1
		|| (_owner->_flags & PollData::Flag::MultiChoice)) {
		resetSendingAnimation();
	}
}

void Poll::Options::repaintAnswersAnimation() const {
	if (!_answersAnimation) {
		return;
	}
	auto &animation = *_answersAnimation;
	if (animation.repaintPending) {
		return;
	}
	animation.repaintPending = true;
	if (animation.repaintRegion.isEmpty()) {
		_owner->repaint();
	} else {
		repaintAnswersAnimationRegion(animation.repaintRegion);
	}
}

void Poll::Options::beginAnswersAnimationPaint(
		const Painter &p,
		const PaintContext &context) const {
	if (!_answersAnimation) {
		return;
	}
	auto &animation = *_answersAnimation;
	if (context.hasElementPainter(p)) {
		animation.repaintPending = false;
		animation.collectedRepaintRegion = QRegion();
		animation.collectingRepaintRegion = true;
	} else {
		if (animation.repaintRegion.isEmpty()) {
			animation.repaintPending = false;
		}
		animation.collectingRepaintRegion = false;
	}
}

void Poll::Options::recordAnswersAnimationRect(
		const Painter &p,
		const PaintContext &context,
		QRect rect) const {
	if (!_answersAnimation
		|| !_answersAnimation->collectingRepaintRegion) {
		return;
	}
	if (const auto mapped = context.mapToElement(p, QRectF(rect))) {
		_answersAnimation->collectedRepaintRegion += *mapped;
	}
}

void Poll::Options::finishAnswersAnimationPaint() const {
	if (!_answersAnimation
		|| !_answersAnimation->collectingRepaintRegion) {
		return;
	}
	auto &animation = *_answersAnimation;
	animation.collectingRepaintRegion = false;
	const auto previous = base::take(animation.repaintRegion);
	animation.repaintRegion = base::take(
		animation.collectedRepaintRegion);
	if (previous == animation.repaintRegion || previous.isEmpty()) {
		return;
	}
	animation.repaintPending = true;
	repaintAnswersAnimationRegion(previous.united(animation.repaintRegion));
}

void Poll::Options::repaintAnswersAnimationRegion(
		const QRegion &region) const {
	_owner->_parent->repaint(region);
}

void Poll::Options::radialAnimationCallback() const {
	if (anim::Disabled() || !_sendingAnimation) {
		return;
	}
	auto &sending = *_sendingAnimation;
	if (sending.repaintPending) {
		return;
	}
	sending.repaintPending = true;
	if (sending.repaintRegion.isEmpty()) {
		_owner->repaint();
	} else {
		repaintSendingAnimationRegion(sending.repaintRegion);
	}
}

void Poll::Options::beginSendingAnimationPaint(
		const Painter &p,
		const PaintContext &context) const {
	if (!_sendingAnimation) {
		return;
	}
	auto &sending = *_sendingAnimation;
	if (context.hasElementPainter(p)) {
		sending.repaintPending = false;
		sending.collectedRepaintRegion = QRegion();
		sending.collectingRepaintRegion = true;
	} else {
		if (sending.repaintRegion.isEmpty()) {
			sending.repaintPending = false;
		}
		sending.collectingRepaintRegion = false;
	}
}

void Poll::Options::recordSendingAnimationRect(
		const Painter &p,
		const PaintContext &context,
		QRectF rect) const {
	if (!_sendingAnimation
		|| !_sendingAnimation->collectingRepaintRegion) {
		return;
	}
	if (const auto mapped = context.mapToElement(p, rect)) {
		_sendingAnimation->collectedRepaintRegion += *mapped;
	}
}

void Poll::Options::finishSendingAnimationPaint() const {
	if (!_sendingAnimation
		|| !_sendingAnimation->collectingRepaintRegion) {
		return;
	}
	auto &sending = *_sendingAnimation;
	sending.collectingRepaintRegion = false;
	const auto previous = base::take(sending.repaintRegion);
	sending.repaintRegion = base::take(sending.collectedRepaintRegion);
	if (previous == sending.repaintRegion || previous.isEmpty()) {
		return;
	}
	sending.repaintPending = true;
	repaintSendingAnimationRegion(previous.united(sending.repaintRegion));
}

void Poll::Options::repaintSendingAnimationRegion(
		const QRegion &region) const {
	_owner->_parent->repaint(region);
}

void Poll::Options::resetSendingAnimation() const {
	if (!_sendingAnimation) {
		return;
	}
	const auto sending = base::take(_sendingAnimation);
	const auto repaintRegion = sending->repaintRegion.united(
		sending->collectedRepaintRegion);
	repaintSendingAnimationRegion(repaintRegion);
}

void Poll::Options::answerTextUpdated(
		const QByteArray &option,
		uint64 generation) const {
	if (_owner->_parent->delegate()->elementAnimationsPaused()) {
		return;
	}
	const auto answer = ranges::find(
		_answers,
		option,
		&Answer::option);
	if (answer == end(_answers)
		|| answer->textGeneration != generation
		|| !HasPollAnswerTextAnimation(answer->text)) {
		return;
	}
	const auto repaint = ranges::find(
		_textRepaints,
		option,
		&TextRepaint::option);
	if (repaint == end(_textRepaints)
		|| repaint->generation != generation) {
		_owner->repaint();
		return;
	}
	if (repaint->repaintPending) {
		return;
	}
	if (repaint->repaintKnown && repaint->repaintRegion.isEmpty()) {
		return;
	}
	repaint->repaintPending = true;
	if (!repaint->repaintKnown) {
		_owner->repaint();
	} else {
		repaintAnswerTextRegion(repaint->repaintRegion);
	}
}

void Poll::Options::invalidateAnswerTextRepaints() {
	for (auto &repaint : _textRepaints) {
		repaint.staleRepaintRegion = repaint.staleRepaintRegion
			.united(base::take(repaint.repaintRegion))
			.united(base::take(repaint.collectedRepaintRegion));
		repaint.repaintPending = false;
		repaint.collectingRepaintRegion = false;
		repaint.repaintKnown = false;
	}
}

void Poll::Options::syncAnswerTextRepaints() {
	for (auto &repaint : _textRepaints) {
		const auto answer = ranges::find(
			_answers,
			repaint.option,
			&Answer::option);
		const auto generation = (answer != end(_answers)
			&& HasPollAnswerTextAnimation(answer->text))
			? answer->textGeneration
			: 0;
		if (repaint.generation == generation) {
			continue;
		}
		repaint.staleRepaintRegion = repaint.staleRepaintRegion
			.united(base::take(repaint.repaintRegion))
			.united(base::take(repaint.collectedRepaintRegion));
		repaint.generation = generation;
		repaint.repaintPending = false;
		repaint.collectingRepaintRegion = false;
		repaint.repaintKnown = false;
	}
	for (const auto &answer : _answers) {
		if (!HasPollAnswerTextAnimation(answer.text)) {
			continue;
		}
		const auto repaint = ranges::find(
			_textRepaints,
			answer.option,
			&TextRepaint::option);
		if (repaint == end(_textRepaints)) {
			_textRepaints.push_back({
				.option = answer.option,
				.generation = answer.textGeneration,
			});
		}
	}
}

void Poll::Options::beginAnswerTextPaint(
		const Painter &p,
		const PaintContext &context) const {
	const auto collect = context.hasElementPainter(p);
	for (auto &repaint : _textRepaints) {
		if (collect) {
			repaint.repaintPending = false;
			repaint.collectedRepaintRegion = QRegion();
			repaint.collectingRepaintRegion = true;
			repaint.collectedRepaintKnown = true;
		} else {
			if (!repaint.repaintKnown
				|| repaint.repaintRegion.isEmpty()) {
				repaint.repaintPending = false;
			}
			repaint.collectingRepaintRegion = false;
		}
	}
}

void Poll::Options::recordAnswerTextRect(
		const Painter &p,
		const PaintContext &context,
		const Answer &answer,
		QRect rect,
		const Ui::Text::CustomEmojiRepaintBounds
			&customEmojiRepaintBounds) const {
	if (!HasPollAnswerTextAnimation(answer.text)) {
		return;
	}
	const auto repaint = ranges::find(
		_textRepaints,
		answer.option,
		&TextRepaint::option);
	if (repaint == end(_textRepaints)
		|| repaint->generation != answer.textGeneration
		|| !repaint->collectingRepaintRegion) {
		return;
	}
	auto region = QRegion();
	repaint->collectedRepaintKnown = repaint->collectedRepaintKnown
		&& AddPollTextRepaintRect(
			region,
			p,
			context,
			answer.text,
			rect,
			customEmojiRepaintBounds);
	if (repaint->collectedRepaintKnown) {
		repaint->collectedRepaintRegion += region;
	}
}

void Poll::Options::finishAnswerTextPaint() const {
	auto repaintRegion = QRegion();
	for (auto i = begin(_textRepaints); i != end(_textRepaints);) {
		if (!i->collectingRepaintRegion) {
			++i;
			continue;
		}
		i->collectingRepaintRegion = false;
		const auto previous = base::take(i->staleRepaintRegion).united(
			base::take(i->repaintRegion));
		i->repaintRegion = i->collectedRepaintKnown
			? base::take(i->collectedRepaintRegion)
			: QRegion();
		i->repaintKnown = i->collectedRepaintKnown;
		if (previous != i->repaintRegion && !previous.isEmpty()) {
			i->repaintPending = true;
			repaintRegion += previous.united(i->repaintRegion);
		}
		if (!i->generation
			&& i->repaintKnown
			&& i->repaintRegion.isEmpty()) {
			i = _textRepaints.erase(i);
		} else {
			++i;
		}
	}
	repaintAnswerTextRegion(repaintRegion);
}

void Poll::Options::repaintAnswerTextRegion(
		const QRegion &region) const {
	_owner->_parent->repaint(region);
}

void Poll::Options::subscribeToThumbnailUpdates(
		not_null<Ui::DynamicImage*> image,
		PollThumbnailKind kind,
		QByteArray option) const {
	const auto raw = image.get();
	image->subscribeToUpdates(crl::guard(_owner, [=] {
		thumbnailUpdated(kind, option, raw);
	}));
}

void Poll::Options::thumbnailUpdated(
		PollThumbnailKind kind,
		const QByteArray &option,
		const Ui::DynamicImage *image) const {
	if (_owner->_parent->delegate()->elementAnimationsPaused()) {
		return;
	}
	if (kind != PollThumbnailKind::Emoji) {
		_owner->repaint();
		return;
	}
	const auto answer = ranges::find(
		_answers,
		option,
		&Answer::option);
	if (answer == end(_answers)
		|| answer->thumbnailKind != PollThumbnailKind::Emoji
		|| answer->thumbnail.get() != image) {
		return;
	}
	const auto repaint = ranges::find(
		_thumbnailRepaints,
		option,
		&ThumbnailRepaint::option);
	if (repaint == end(_thumbnailRepaints)
		|| repaint->image != image) {
		_owner->repaint();
		return;
	}
	if (repaint->repaintPending) {
		return;
	}
	repaint->repaintPending = true;
	if (repaint->repaintRegion.isEmpty()) {
		_owner->repaint();
	} else {
		repaintAnimatedThumbnailRegion(repaint->repaintRegion);
	}
}

void Poll::Options::invalidateAnimatedThumbnailRepaints() {
	for (auto &repaint : _thumbnailRepaints) {
		repaint.staleRepaintRegion = repaint.staleRepaintRegion
			.united(base::take(repaint.repaintRegion))
			.united(base::take(repaint.collectedRepaintRegion));
		repaint.repaintPending = false;
		repaint.collectingRepaintRegion = false;
	}
}

void Poll::Options::syncAnimatedThumbnailRepaints() {
	for (auto &repaint : _thumbnailRepaints) {
		const auto answer = ranges::find(
			_answers,
			repaint.option,
			&Answer::option);
		const auto image = (answer != end(_answers)
			&& answer->thumbnailKind == PollThumbnailKind::Emoji)
			? answer->thumbnail.get()
			: nullptr;
		if (repaint.image == image) {
			continue;
		}
		repaint.staleRepaintRegion = repaint.staleRepaintRegion
			.united(base::take(repaint.repaintRegion))
			.united(base::take(repaint.collectedRepaintRegion));
		repaint.image = image;
		repaint.repaintPending = false;
		repaint.collectingRepaintRegion = false;
	}
	for (const auto &answer : _answers) {
		if (answer.thumbnailKind != PollThumbnailKind::Emoji
			|| !answer.thumbnail) {
			continue;
		}
		const auto repaint = ranges::find(
			_thumbnailRepaints,
			answer.option,
			&ThumbnailRepaint::option);
		if (repaint == end(_thumbnailRepaints)) {
			_thumbnailRepaints.push_back({
				.option = answer.option,
				.image = answer.thumbnail.get(),
			});
		}
	}
}

void Poll::Options::beginAnimatedThumbnailPaint(
		const Painter &p,
		const PaintContext &context) const {
	const auto collect = context.hasElementPainter(p);
	for (auto &repaint : _thumbnailRepaints) {
		if (collect) {
			repaint.repaintPending = false;
			repaint.collectedRepaintRegion = QRegion();
			repaint.collectingRepaintRegion = true;
		} else {
			if (repaint.repaintRegion.isEmpty()) {
				repaint.repaintPending = false;
			}
			repaint.collectingRepaintRegion = false;
		}
	}
}

void Poll::Options::recordAnimatedThumbnailRect(
		const Painter &p,
		const PaintContext &context,
		const Answer &answer,
		QRect rect) const {
	if (answer.thumbnailKind != PollThumbnailKind::Emoji) {
		return;
	}
	const auto repaint = ranges::find(
		_thumbnailRepaints,
		answer.option,
		&ThumbnailRepaint::option);
	if (repaint == end(_thumbnailRepaints)
		|| repaint->image != answer.thumbnail.get()
		|| !repaint->collectingRepaintRegion) {
		return;
	}
	const auto mapped = context.mapToElement(p, QRectF(rect));
	if (mapped) {
		repaint->collectedRepaintRegion += *mapped;
	}
}

void Poll::Options::finishAnimatedThumbnailPaint() const {
	auto repaintRegion = QRegion();
	for (auto i = begin(_thumbnailRepaints);
			i != end(_thumbnailRepaints);) {
		if (!i->collectingRepaintRegion) {
			++i;
			continue;
		}
		i->collectingRepaintRegion = false;
		const auto previous = base::take(i->staleRepaintRegion).united(
			base::take(i->repaintRegion));
		i->repaintRegion = base::take(i->collectedRepaintRegion);
		if (previous != i->repaintRegion && !previous.isEmpty()) {
			i->repaintPending = true;
			repaintRegion += previous.united(i->repaintRegion);
		}
		if (!i->image && i->repaintRegion.isEmpty()) {
			i = _thumbnailRepaints.erase(i);
		} else {
			++i;
		}
	}
	repaintAnimatedThumbnailRegion(repaintRegion);
}

void Poll::Options::repaintAnimatedThumbnailRegion(
		const QRegion &region) const {
	_owner->_parent->repaint(region);
}

void Poll::Options::recordAnswerFeedbackRect(
		const Painter &p,
		const PaintContext &context,
		const Answer &answer,
		FeedbackPart part,
		QRect rect) const {
	_owner->recordRepaintGeometry(
		feedbackRepaint(ensureFeedbackRepaints(answer.option), part),
		p,
		context,
		QRegion(rect));
}

Poll::Options::FeedbackRepaints &Poll::Options::ensureFeedbackRepaints(
		const QByteArray &option) const {
	const auto i = ranges::find(
		_feedbackRepaints,
		option,
		&FeedbackRepaints::option);
	if (i != end(_feedbackRepaints)) {
		return *i;
	}
	_feedbackRepaints.push_back({ .option = option });
	return _feedbackRepaints.back();
}

Poll::Options::FeedbackRepaints *Poll::Options::findFeedbackRepaints(
		const QByteArray &option) const {
	const auto i = ranges::find(
		_feedbackRepaints,
		option,
		&FeedbackRepaints::option);
	return (i == end(_feedbackRepaints)) ? nullptr : &*i;
}

Poll::RepaintState &Poll::Options::feedbackRepaint(
		FeedbackRepaints &repaints,
		FeedbackPart part) const {
	switch (part) {
	case FeedbackPart::Toggle: return repaints.toggle;
	case FeedbackPart::Ripple: return repaints.ripple;
	}
	Unexpected("Feedback part in Poll::Options::feedbackRepaint.");
}

uint64 Poll::Options::resetFeedbackRepaint(
		const QByteArray &option,
		FeedbackPart part) const {
	return _owner->startRepaintGeneration(
		feedbackRepaint(ensureFeedbackRepaints(option), part));
}

void Poll::Options::repaintAnswerFeedback(
		const QByteArray &option,
		FeedbackPart part,
		uint64 generation) const {
	if (const auto repaints = findFeedbackRepaints(option)) {
		_owner->repaintGeometry(
			feedbackRepaint(*repaints, part),
			generation);
	}
}

void Poll::Options::repaintAnswerFeedback(
		const QByteArray &option,
		FeedbackPart part) const {
	_owner->repaintGeometry(
		feedbackRepaint(ensureFeedbackRepaints(option), part));
}

void Poll::Options::invalidateFeedbackRepaints() const {
	for (auto &repaints : _feedbackRepaints) {
		_owner->invalidateRepaintGeometry(repaints.toggle);
		_owner->invalidateRepaintGeometry(repaints.ripple);
	}
}

void Poll::Options::syncFeedbackRepaints() const {
	for (auto &repaints : _feedbackRepaints) {
		if (ranges::find(
				_answers,
				repaints.option,
				&Answer::option) == end(_answers)) {
			_owner->stopRepaintGeneration(repaints.toggle);
			_owner->stopRepaintGeneration(repaints.ripple);
		}
	}
	for (const auto &answer : _answers) {
		ensureFeedbackRepaints(answer.option);
	}
}

void Poll::Options::finishFeedbackRepaints() const {
	for (auto i = begin(_feedbackRepaints);
			i != end(_feedbackRepaints);) {
		if (ranges::find(
				_answers,
				i->option,
				&Answer::option) != end(_answers)) {
			++i;
			continue;
		}
		_owner->recordRepaintGeometry(i->toggle, QRegion(), true);
		_owner->recordRepaintGeometry(i->ripple, QRegion(), true);
		i = _feedbackRepaints.erase(i);
	}
}

void Poll::Options::resetAnswerRipple(const Answer &answer) const {
	if (const auto repaints = findFeedbackRepaints(answer.option)) {
		_owner->stopRepaintGeneration(repaints->ripple);
	}
	answer.ripple.reset();
	answer.rippleMaskSize = QSize();
}

void Poll::Header::paintRecentVoters(
		Painter &p,
		int left,
		int top,
		const PaintContext &context) const {
	const auto count = int(_recentVoters.size());
	if (!count) {
		return;
	}
	auto x = left
		+ st::historyPollRecentVotersSkip
		+ (count - 1) * st::historyPollRecentVoterSkip;
	auto y = top;
	const auto size = st::historyPollRecentVoterSize;
	const auto stm = context.messageStyle();
	auto pen = stm->msgBg->p;
	pen.setWidth(st::lineWidth);

	auto created = false;
	for (const auto &recent : ranges::views::reverse(_recentVoters)) {
		const auto was = !recent.userpic.null();
		recent.peer->paintUserpic(p, recent.userpic, x, y, size);
		if (!was && !recent.userpic.null()) {
			created = true;
		}
		const auto paintContent = [&](QPainter &p) {
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(x, y, size, size);
		};
		if (_owner->usesBubblePattern(context)) {
			const auto add = st::lineWidth * 2;
			const auto target = QRect(x, y, size, size).marginsAdded(
				{ add, add, add, add });
			Ui::PaintPatternBubblePart(
				p,
				context.viewport,
				context.bubblesPattern->pixmap,
				target,
				paintContent,
				_userpicCircleCache);
		} else {
			paintContent(p);
		}
		x -= st::historyPollRecentVoterSkip;
	}
	if (created) {
		_owner->history()->owner().registerHeavyViewPart(_owner->_parent);
	}
}

void Poll::Header::paintShowSolution(
		Painter &p,
		int right,
		int top,
		const PaintContext &context) const {
	const auto shown = _solutionButtonAnimation.value(
		_solutionButtonVisible ? 1. : 0.);
	const auto stm = context.messageStyle();
	const auto &icon = stm->historyQuizExplain;
	const auto x = right - icon.width();
	const auto y = top + (st::normalFont->height - icon.height()) / 2;
	const auto target = style::rtlrect(
		x,
		y,
		icon.width(),
		icon.height(),
		_owner->width());
	_owner->recordRepaintGeometry(
		_solutionButtonRepaint,
		p,
		context,
		QRegion(target));
	if (!shown) {
		if (!_solutionButtonAnimation.animating()) {
			_owner->stopRepaintGeneration(_solutionButtonRepaint);
		}
		return;
	}
	if (!_showSolutionLink) {
		_showSolutionLink = std::make_shared<LambdaClickHandler>(
			crl::guard(_owner, [=] { _owner->_headerPart->showSolution(); }));
	}
	if (shown == 1.) {
		icon.paint(p, x, y, _owner->width());
	} else {
		p.save();
		const auto center = QRectF(target).center();
		p.translate(center);
		p.scale(shown, shown);
		p.translate(-center);
		p.setOpacity(shown);
		icon.paint(p, x, y, _owner->width());
		p.restore();
	}
	if (!_solutionButtonAnimation.animating()) {
		_owner->stopRepaintGeneration(_solutionButtonRepaint);
	}
}

void Poll::Header::paintSolutionBlock(
		Painter &p,
		int left,
		int top,
		int paintw,
		const PaintContext &context) const {
	if (!_solutionShown || !canShowSolution()) {
		return;
	}
	if (!_closeSolutionLink) {
		_closeSolutionLink = std::make_shared<LambdaClickHandler>(
			crl::guard(
				_owner,
				[=] { _owner->_headerPart->solutionToggled(false); }));
	}

	const auto &qst = st::historyPagePreview;
	const auto blockHeight = countSolutionBlockHeight(paintw);
	const auto outer = QRect(left, top, paintw, blockHeight);

	const auto stm = context.messageStyle();
	const auto view = _owner->_parent;
	const auto selected = context.selected();
	const auto colorIndex = view->contentColorIndex();
	const auto &chatSt = *context.st;
	const auto colorPattern = chatSt.colorPatternIndex(colorIndex);
	const auto useColorIndex = !context.outbg;
	const auto cache = useColorIndex
		? chatSt.coloredReplyCache(selected, colorIndex).get()
		: stm->replyCache[colorPattern].get();

	Ui::Text::ValidateQuotePaintCache(*cache, qst);
	Ui::Text::FillQuotePaint(p, outer, *cache, qst);

	const auto innerLeft = left + qst.padding.left();
	const auto innerRight = left + paintw - qst.padding.right();
	const auto textWidth = innerRight - innerLeft;
	auto yshift = top + qst.padding.top();

	p.setPen(cache->outlines[0]);
	p.setFont(st::semiboldFont);
	const auto closeArea = st::historyPollExplanationCloseSize;
	p.drawTextLeft(
		innerLeft,
		yshift,
		_owner->width(),
		tr::lng_polls_solution_title(tr::now),
		textWidth - closeArea);

	{
		const auto iconSize = st::historyPollExplanationCloseIconSize;
		const auto centerX = innerRight - closeArea / 2;
		const auto centerY = yshift + st::semiboldFont->height / 2;
		const auto half = iconSize / 2;
		auto pen = QPen(cache->outlines[0]);
		pen.setWidthF(st::historyPollExplanationCloseStroke * 1.);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setRenderHint(QPainter::Antialiasing);
		p.drawLine(
			centerX - half, centerY - half,
			centerX + half, centerY + half);
		p.drawLine(
			centerX + half, centerY - half,
			centerX - half, centerY + half);
		p.setRenderHint(QPainter::Antialiasing, false);
	}

	yshift += st::semiboldFont->height + st::historyPollExplanationTitleSkip;

	auto solutionRepaintRegion = QRegion();
	auto solutionRepaintKnown = context.hasElementPainter(p);
	const auto solutionTextHeight = _solutionText.countHeight(textWidth);
	p.setPen(stm->historyTextFg);
	_owner->_parent->prepareCustomEmojiPaint(p, context, _solutionText);
	auto customEmojiRepaintBounds
		= Ui::Text::CustomEmojiRepaintBounds();
	_solutionText.draw(p, {
		.position = { innerLeft, yshift },
		.outerWidth = _owner->width(),
		.availableWidth = textWidth,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.pausedEmoji = context.paused,
		.pausedSpoiler = context.paused,
		.selection = toSolutionSelection(context.selection),
		.customEmojiRepaintBounds = &customEmojiRepaintBounds,
	});
	solutionRepaintKnown = solutionRepaintKnown
		&& AddPollTextRepaintRect(
			solutionRepaintRegion,
			p,
			context,
			_solutionText,
			QRect(innerLeft, yshift, textWidth, solutionTextHeight),
			customEmojiRepaintBounds);
	finishTextRepaint(
		TextPart::Solution,
		p,
		context,
		std::move(solutionRepaintRegion),
		solutionRepaintKnown);

	if (countSolutionMediaHeight(textWidth)) {
		yshift += solutionTextHeight
			+ st::historyPollExplanationMediaSkip;
		const auto isDocument = _solutionMedia
			&& (_solutionMedia->kind == PollThumbnailKind::Document
				|| _solutionMedia->kind == PollThumbnailKind::Audio);
		const auto isThumbed = isDocument
			&& _owner->_poll->solutionMedia.document
			&& _owner->_poll->solutionMedia.document->hasThumbnail()
			&& !_owner->_poll->solutionMedia.document->isSong();
		const auto &fileSt = isThumbed
			? st::msgFileThumbLayout
			: st::msgFileLayout;
		const auto shift = isDocument ? fileSt.padding.left() : 0;
		const auto attachLeft = rtl()
			? (_owner->width() - innerLeft + shift - _solutionAttach->width())
			: (innerLeft - shift);
		p.translate(attachLeft, yshift);
		_solutionAttach->draw(
			p,
			context.translated(-attachLeft, -yshift)
				.withSelection(TextSelection()));
		p.translate(-attachLeft, -yshift);
	}
}

int Poll::Options::paintAnswer(
		Painter &p,
		const Answer &answer,
		const AnswerAnimation *animation,
		int left,
		int top,
		int width,
		int outerWidth,
		const PaintContext &context) const {
	const auto height = countAnswerHeight(answer, width);
	const auto rippleSize = QSize(outerWidth, height);
	if (answer.ripple && answer.rippleMaskSize != rippleSize) {
		resetAnswerRipple(answer);
	}
	const auto &answerPadding = answer.thumbnail
		? st::historyPollAnswerPadding
		: st::historyPollAnswerPaddingNoMedia;
	recordAnswerFeedbackRect(
		p,
		context,
		answer,
		FeedbackPart::Ripple,
		QRect(0, top, outerWidth, height));
	recordAnswerFeedbackRect(
		p,
		context,
		answer,
		FeedbackPart::Toggle,
		QRect(
			left,
			top + answerPadding.top(),
			st::historyPollRadio.diameter,
			st::historyPollRadio.diameter).marginsAdded(
				Margins(st::lineWidth)));
	if (!context.highlight.pollOption.isEmpty()
		&& context.highlight.pollOption == answer.option
		&& context.highlight.collapsion > 0.) {
		const auto hlTextWidth = countAnswerContentWidth(answer, width);
		const auto hlTextHeight = answer.text.countHeight(hlTextWidth);
		const auto hlMultiline = (hlTextHeight
			> st::historyPollPercentFont->height);
		const auto hlVotesExtra = countVotesExtraHeight(
			answer,
			hlTextWidth);
		const auto fillingExtra = (_owner->showVotes()
			&& !answer.thumbnail
			&& !hlMultiline
			&& !hlVotesExtra)
			? (st::historyPollChoiceRight.height() / 2)
			: 0;
		const auto absoluteTop = top
			+ _owner->_headerPart->countHeight(width);
		const auto to = context.highlightInterpolateTo;
		const auto toProgress = (1. - context.highlight.collapsion);
		if (toProgress >= 1.) {
			context.highlightPathCache->addRect(to);
		} else if (toProgress <= 0.) {
			context.highlightPathCache->addRect(
				0,
				absoluteTop + fillingExtra,
				_owner->width(),
				height + fillingExtra);
		} else {
			const auto lerp = [=](int from, int to) {
				return from + (to - from) * toProgress;
			};
			context.highlightPathCache->addRect(
				lerp(0, to.x()),
				lerp(absoluteTop, to.y()) + fillingExtra,
				lerp(_owner->width(), to.width()),
				lerp(height + fillingExtra, to.height()));
		}
	}
	const auto stm = context.messageStyle();
	const auto aleft = left + st::historyPollAnswerPadding.left();
	const auto awidth = width
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();
	const auto media = answer.thumbnail ? PollAnswerMediaSize() : 0;
	const auto textWidth = countAnswerContentWidth(answer, width);
	const auto textContentHeight = answer.text.countHeight(textWidth);
	const auto multilineAnswer = (textContentHeight
		> st::historyPollPercentFont->height);
	const auto votesExtraHeight = countVotesExtraHeight(
		answer,
		textWidth);
	const auto fillingContentHeight = (multilineAnswer || votesExtraHeight)
		? (textContentHeight + votesExtraHeight)
		: textContentHeight;
	const auto anyMediaWidth = _anyAnswerHasMedia
		? (PollAnswerMediaSize() + PollAnswerMediaSkip())
		: 0;
	const auto barContentWidth = std::max(1, awidth - anyMediaWidth);

	if (answer.ripple) {
		p.setOpacity(st::historyPollRippleOpacity);
		answer.ripple->paint(
			p,
			left - st::msgPadding.left(),
			top,
			outerWidth,
			&stm->msgWaveformInactive->c);
		if (answer.ripple->empty()) {
			resetAnswerRipple(answer);
		}
		p.setOpacity(1.);
	}

	const auto paintVotesCount = [&](float64 opacity = 1.) {
		if (answer.votesCountString.isEmpty()) {
			return;
		}
		if (!answer.recentVoters.empty()) {
			if (NeedRegenerateUserpics(
					answer.recentVotersImage,
					answer.recentVoters)) {
				const auto was = !answer.recentVotersImage.isNull();
				GenerateUserpicsInRow(
					answer.recentVotersImage,
					answer.recentVoters,
					st::historyPollAnswerUserpics);
				if (!was) {
					_owner->history()->owner().registerHeavyViewPart(
						_owner->_parent);
				}
			}
		}
		const auto userpicsWidth = answer.recentVotersImage.isNull()
			? 0
			: (answer.recentVotersImage.width()
				/ style::DevicePixelRatio());
		const auto userpicsExtra = userpicsWidth
			? (st::historyPollAnswerUserpicsSkip + userpicsWidth)
			: 0;
		const auto rightEdge = aleft + awidth
			- anyMediaWidth
			- st::historyPollFillingRight;
		const auto countX = rightEdge
			- answer.votesCountWidth
			- userpicsExtra;
		const auto atop = top + answerPadding.top()
			+ ((multilineAnswer || votesExtraHeight)
				? (textContentHeight
					- (votesExtraHeight ? 0 : st::normalFont->height))
				: 0);
		p.setOpacity(opacity);
		p.setFont(st::normalFont);
		p.setPen(stm->msgDateFg);
		p.drawTextLeft(
			countX,
			atop,
			outerWidth,
			answer.votesCountString,
			answer.votesCountWidth);
		if (userpicsWidth) {
			const auto userpicsX = rightEdge - userpicsWidth;
			const auto userpicsY = atop
				+ (st::normalFont->height
					- st::historyPollAnswerUserpics.size) / 2;
			p.drawImage(userpicsX, userpicsY, answer.recentVotersImage);
		}
	};

	if (animation) {
		const auto opacity = animation->opacity.current();
		if (opacity < 1.) {
			p.setOpacity(1. - opacity);
			paintRadio(p, answer, left, top, context);
		}
		if (opacity > 0.) {
			const auto percent = QString::number(
				int(base::SafeRound(animation->percent.current()))) + '%';
			const auto percentWidth = st::historyPollPercentFont->width(
				percent);
			p.setOpacity(opacity);
			paintPercent(
				p,
				percent,
				percentWidth,
				left,
				top,
				answerPadding.top(),
				outerWidth,
				context);
			paintVotesCount(opacity);
			p.setOpacity(sqrt(opacity));
			paintFilling(
				p,
				animation->chosen,
				animation->correct,
				animation->filling.current(),
				left,
				top,
				answerPadding.top(),
				width,
				barContentWidth,
				fillingContentHeight,
				context);
			p.setOpacity(1.);
		}
	} else if (!_owner->showVotes()) {
		paintRadio(p, answer, left, top, context);
	} else {
		paintPercent(
			p,
			answer.votesPercentString,
			answer.votesPercentWidth,
			left,
			top,
			answerPadding.top(),
			outerWidth,
			context);
		paintVotesCount();
		paintFilling(
			p,
			answer.chosen,
			answer.correct,
			answer.filling,
			left,
			top,
			answerPadding.top(),
			width,
			barContentWidth,
			fillingContentHeight,
			context);
	}

	top += answerPadding.top();
	if (answer.thumbnail) {
		const auto target = QRect(
			aleft + awidth - media,
			top,
			media,
			media);
		if (!target.isEmpty()) {
			recordAnimatedThumbnailRect(p, context, answer, target);
			const auto webpagePlaceholder
				= (answer.thumbnailKind == PollThumbnailKind::Webpage)
					&& !answer.thumbnailId;
			const auto selected = context.selected();
			const auto &linkIcon = context.outbg
				? (selected
					? st::historyPollLinkOutIconSelected
					: st::historyPollLinkOutIcon)
				: (selected
					? st::historyPollLinkInIconSelected
					: st::historyPollLinkInIcon);
			if (webpagePlaceholder) {
				const auto cache = stm->replyCache[0].get();
				p.save();
				auto hq = PainterHighQualityEnabler(p);
				auto path = QPainterPath();
				path.addRoundedRect(
					target,
					st::historyPollAnswerThumbRadius,
					st::historyPollAnswerThumbRadius);
				p.fillPath(path, cache->bg);
				if (selected) {
					p.setClipPath(path);
					p.fillRect(target, context.st->msgSelectOverlay());
				}
				p.restore();
				const auto iconX = target.x()
					+ (target.width() - linkIcon.width()) / 2;
				const auto iconY = target.y()
					+ (target.height() - linkIcon.height()) / 2;
				linkIcon.paint(p, iconX, iconY, outerWidth, cache->icon);
			} else {
				const auto image = answer.thumbnail->image(media);
				if (!image.isNull()) {
					const auto source = QRectF(
						QPointF(),
						QSizeF(image.size()));
					const auto kx = target.width() / source.width();
					const auto ky = target.height() / source.height();
					const auto scale = std::max(kx, ky);
					const auto size = QSizeF(
						source.width() * scale,
						source.height() * scale);
					const auto geometry = QRectF(
						target.x() + (target.width() - size.width()) / 2.,
						target.y() + (target.height() - size.height()) / 2.,
						size.width(),
						size.height());
					p.save();
					auto hq = PainterHighQualityEnabler(p);
					if (answer.thumbnailRounded) {
						auto path = QPainterPath();
						path.addRoundedRect(
							target,
							st::historyPollAnswerThumbRadius,
							st::historyPollAnswerThumbRadius);
						p.setClipPath(path);
					}
					p.drawImage(geometry, image, source);
					p.restore();
					if (answer.thumbnailIsVideo) {
						st::dialogsMiniPlay.paintInCenter(p, target);
					}
					if (answer.thumbnailKind
						== PollThumbnailKind::Webpage) {
						p.save();
						auto hq = PainterHighQualityEnabler(p);
						auto path = QPainterPath();
						path.addRoundedRect(
							target,
							st::historyPollAnswerThumbRadius,
							st::historyPollAnswerThumbRadius);
						p.setClipPath(path);
						p.fillRect(target, st::songCoverOverlayFg);
						linkIcon.paintInCenter(p, target);
						p.restore();
					}
				}
			}
		}
	}
	p.setPen(stm->historyTextFg);
	_owner->_parent->prepareCustomEmojiPaint(p, context, answer.text);
	auto customEmojiRepaintBounds
		= Ui::Text::CustomEmojiRepaintBounds();
	answer.text.draw(p, {
		.position = { aleft, top },
		.outerWidth = outerWidth,
		.availableWidth = textWidth,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.pausedEmoji = context.paused,
		.pausedSpoiler = context.paused,
		.customEmojiRepaintBounds = &customEmojiRepaintBounds,
	});
	recordAnswerTextRect(
		p,
		context,
		answer,
		QRect(aleft, top, textWidth, textContentHeight),
		customEmojiRepaintBounds);

	return height;
}

void Poll::Options::paintRadio(
		Painter &p,
		const Answer &answer,
		int left,
		int top,
		const PaintContext &context) const {
	const auto &answerPadding = answer.thumbnail
		? st::historyPollAnswerPadding
		: st::historyPollAnswerPaddingNoMedia;
	top += answerPadding.top();

	const auto stm = context.messageStyle();

	PainterHighQualityEnabler hq(p);
	const auto &radio = st::historyPollRadio;
	const auto over = ClickHandler::showAsActive(answer.handler);
	const auto &regular = stm->msgDateFg;

	const auto chosen = answer.chosen;
	const auto checkmark = chosen
		? 1.
		: answer.selectedAnimation.value(answer.selected ? 1. : 0.);
	if (!answer.selectedAnimation.animating()) {
		if (const auto repaints = findFeedbackRepaints(answer.option)) {
			_owner->stopRepaintGeneration(repaints->toggle);
		}
	}

	const auto o = p.opacity();
	if (checkmark < 1.) {
		p.setBrush(Qt::NoBrush);
		p.setOpacity(o
			* (over
				? st::historyPollRadioOpacityOver
				: st::historyPollRadioOpacity));
	}

	const auto multiChoice = (_owner->_flags & PollData::Flag::MultiChoice);
	const auto rect = QRectF(left, top, radio.diameter, radio.diameter)
		- Margins(radio.thickness / 2.);
	const auto radius = st::historyPollCheckboxRadius;
	if (_sendingAnimation && _sendingAnimation->option == answer.option) {
		recordSendingAnimationRect(
			p,
			context,
			QRectF(left, top, radio.diameter, radio.diameter)
				.marginsAdded(Margins(st::lineWidth)));
		const auto &active = stm->msgServiceFg;
		if (anim::Disabled()) {
			anim::DrawStaticLoading(p, rect, radio.thickness, active);
		} else {
			const auto state = _sendingAnimation->animation.computeState();
			auto pen = anim::pen(regular, active, state.shown);
			pen.setWidth(radio.thickness);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			if (multiChoice) {
				p.drawRoundedRect(rect, radius, radius);
			} else {
				p.drawArc(
					rect,
					state.arcFrom,
					state.arcLength);
			}
		}
	} else {
		if (checkmark < 1.) {
			auto pen = regular->p;
			pen.setWidth(radio.thickness);
			p.setPen(pen);
			if (multiChoice) {
				p.drawRoundedRect(rect, radius, radius);
			} else {
				p.drawEllipse(rect);
			}
		}
		if (checkmark > 0.) {
			const auto removeFull = (radio.diameter / 2 - radio.thickness);
			const auto removeNow = removeFull * (1. - checkmark);
			const auto color = stm->msgFileThumbLinkFg;
			auto pen = color->p;
			pen.setWidth(radio.thickness);
			p.setPen(pen);
			p.setBrush(color);
			const auto inner = rect - Margins(removeNow);
			if (multiChoice) {
				p.drawRoundedRect(inner, radius, radius);
			} else {
				p.drawEllipse(inner);
			}
			const auto &icon = stm->historyPollChosen;
			icon.paint(
				p,
				left + (radio.diameter - icon.width()) / 2,
				top + (radio.diameter - icon.height()) / 2,
				_owner->width());
		}
	}

	p.setOpacity(o);
}

void Poll::Options::paintPercent(
		Painter &p,
		const QString &percent,
		int percentWidth,
		int left,
		int top,
		int topPadding,
		int outerWidth,
		const PaintContext &context) const {
	const auto stm = context.messageStyle();
	const auto aleft = left + st::historyPollAnswerPadding.left();

	top += topPadding;

	p.setFont(st::historyPollPercentFont);
	p.setPen(stm->historyTextFg);
	const auto pleft = aleft - percentWidth - st::historyPollPercentSkip;
	p.drawTextLeft(
		pleft,
		top + st::historyPollPercentTop,
		outerWidth,
		percent,
		percentWidth);
}

void Poll::Options::paintFilling(
		Painter &p,
		bool chosen,
		bool correct,
		float64 filling,
		int left,
		int top,
		int topPadding,
		int width,
		int contentWidth,
		int contentHeight,
		const PaintContext &context) const {
	const auto st = context.st;
	const auto stm = context.messageStyle();
	const auto aleft = left + st::historyPollAnswerPadding.left();

	top += topPadding;

	const auto thickness = st::historyPollFillingHeight;
	const auto max = contentWidth - st::historyPollFillingRight;
	const auto size
		= anim::interpolate(st::historyPollFillingMin, max, filling);
	const auto radius = st::historyPollFillingRadius;
	const auto ftop = top
		+ std::max(st::historyPollPercentFont->height, contentHeight)
		+ st::historyPollFillingTop;

	enum class Style {
		Incorrect,
		Correct,
		Default,
	};
	const auto style = [&] {
		if (chosen && !correct) {
			return Style::Incorrect;
		} else if (chosen
			&& correct
			&& _owner->_poll->quiz()
			&& !context.outbg) {
			return Style::Correct;
		} else {
			return Style::Default;
		}
	}();
	auto barleft = aleft;
	auto barwidth = size;
	const auto &color = (style == Style::Incorrect)
		? st->boxTextFgError()
		: (style == Style::Correct)
		? st->boxTextFgGood()
		: stm->msgFileBg;
	p.setPen(Qt::NoPen);
	PainterHighQualityEnabler hq(p);
	{
		p.setBrush(anim::with_alpha(color->c, st::historyPollFillingBgOpacity));
		p.drawRoundedRect(barleft, ftop, max, thickness, radius, radius);
	}
	p.setBrush(color);
	if (chosen || correct) {
		const auto &icon = (style == Style::Incorrect)
			? st->historyPollChoiceWrong()
			: (style == Style::Correct)
			? st->historyPollChoiceRight()
			: stm->historyPollChoiceRight;
		const auto cleft = aleft - st::historyPollPercentSkip - icon.width();
		const auto ctop = ftop - (icon.height() - thickness) / 2;
		if (_owner->_flags & PollData::Flag::MultiChoice) {
			p.drawRoundedRect(
				cleft,
				ctop,
				icon.width(),
				icon.height(),
				st::historyPollCheckboxRadius,
				st::historyPollCheckboxRadius);
		} else {
			p.drawEllipse(cleft, ctop, icon.width(), icon.height());
		}

		const auto paintContent = [&](QPainter &p) {
			icon.paint(p, cleft, ctop, width);
		};
		if (style == Style::Default && _owner->usesBubblePattern(context)) {
			const auto add = st::lineWidth * 2;
			const auto target = QRect(
				cleft,
				ctop,
				icon.width(),
				icon.height()
			).marginsAdded({ add, add, add, add });
			Ui::PaintPatternBubblePart(
				p,
				context.viewport,
				context.bubblesPattern->pixmap,
				target,
				paintContent,
				_fillingIconCache);
		} else {
			paintContent(p);
		}
		//barleft += icon.width() - radius;
		//barwidth -= icon.width() - radius;
	}
	if (barwidth > 0) {
		p.drawRoundedRect(barleft, ftop, barwidth, thickness, radius, radius);
	}
}

bool Poll::Options::answerVotesChanged() const {
	if (_owner->_poll->answers.size() != _answers.size()
		|| _owner->_poll->answers.empty()) {
		return false;
	}
	for (const auto &answer : _answers) {
		const auto i = ranges::find(
			_owner->_poll->answers,
			answer.option,
			&PollAnswer::option);
		if (i == end(_owner->_poll->answers)) {
			return false;
		} else if (answer.votes != i->votes) {
			return true;
		}
	}
	return false;
}

void Poll::Options::saveStateInAnimation() const {
	if (_answersAnimation) {
		return;
	}
	const auto show = _owner->showVotes();
	_answersAnimation = std::make_unique<AnswersAnimation>();
	_answersAnimation->data.reserve(_answers.size());
	const auto convert = [&](const Answer &answer) {
		auto result = AnswerAnimation();
		result.percent = show ? float64(answer.votesPercent) : 0.;
		result.filling = show ? answer.filling : 0.;
		result.opacity = show ? 1. : 0.;
		result.chosen = answer.chosen;
		result.correct = answer.correct;
		return result;
	};
	ranges::transform(
		_answers,
		ranges::back_inserter(_answersAnimation->data),
		convert);
}

bool Poll::Options::checkAnimationStart() const {
	if (_owner->_poll->answers.size() != _answers.size()) {
		// Skip initial changes.
		return false;
	}
	const auto result = _owner->showVotes()
		!= (_owner->_poll->voted() || _owner->_poll->closed())
		|| answerVotesChanged();
	if (result) {
		saveStateInAnimation();
	}
	return result;
}

void Poll::Options::startAnswersAnimation() const {
	if (!_answersAnimation) {
		return;
	}

	const auto show = _owner->showVotes();
	auto &&both = ranges::views::zip(_answers, _answersAnimation->data);
	for (auto &&[answer, data] : both) {
		data.percent.start(show ? float64(answer.votesPercent) : 0.);
		data.filling.start(show ? answer.filling : 0.);
		data.opacity.start(show ? 1. : 0.);
		data.chosen = data.chosen || answer.chosen;
		data.correct = data.correct || answer.correct;
	}
	_answersAnimation->progress.start(
		[=] { repaintAnswersAnimation(); },
		0.,
		1.,
		st::historyPollDuration);
}

TextSelection Poll::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	const auto descLen = _headerPart->_description.length();
	const auto solLen = _headerPart->solutionSelectionLength();
	const auto descSolLen = uint16(descLen + solLen);

	if (descLen == 0 && solLen == 0) {
		return _headerPart->_question.adjustSelection(selection, type);
	}
	if (selection.to <= descLen && descLen > 0) {
		return _headerPart->_description.adjustSelection(selection, type);
	}
	if (solLen > 0
		&& selection.from >= descLen
		&& selection.to <= descSolLen) {
		const auto adjusted = _headerPart->_solutionText.adjustSelection(
			_headerPart->toSolutionSelection(selection),
			type);
		return _headerPart->fromSolutionSelection(adjusted);
	}
	const auto questionSelection = _headerPart->_question.adjustSelection(
		_headerPart->toQuestionSelection(selection),
		type);
	if (selection.from >= descSolLen) {
		return _headerPart->fromQuestionSelection(questionSelection);
	}
	const auto from = (selection.from < descLen && descLen > 0)
		? _headerPart->_description.adjustSelection(selection, type).from
		: (solLen > 0 && selection.from < descSolLen)
		? _headerPart->fromSolutionSelection(
			_headerPart->_solutionText.adjustSelection(
				_headerPart->toSolutionSelection(selection),
				type)).from
		: _headerPart->fromQuestionSelection(questionSelection).from;
	const auto to = (selection.to <= descSolLen && solLen > 0)
		? _headerPart->fromSolutionSelection(
			_headerPart->_solutionText.adjustSelection(
				_headerPart->toSolutionSelection(selection),
				type)).to
		: _headerPart->fromQuestionSelection(questionSelection).to;
	return { from, to };
}

uint16 Poll::fullSelectionLength() const {
	return _headerPart->_description.length()
		+ _headerPart->solutionSelectionLength()
		+ _headerPart->_question.length();
}

TextForMimeData Poll::selectedText(TextSelection selection) const {
	auto description = _headerPart->_description.toTextForMimeData(selection);
	auto solution = _headerPart->_solutionText.toTextForMimeData(
		_headerPart->toSolutionSelection(selection));
	auto question = _headerPart->_question.toTextForMimeData(
		_headerPart->toQuestionSelection(selection));
	auto result = TextForMimeData();
	const auto append = [&](TextForMimeData &&part) {
		if (part.empty()) {
			return;
		}
		if (!result.empty()) {
			result.append('\n');
		}
		result.append(std::move(part));
	};
	append(std::move(description));
	append(std::move(solution));
	append(std::move(question));
	return result;
}

TextState Poll::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	if (!_poll->sendingVotes.empty()) {
		return result;
	}

	const auto padding = st::msgPadding;
	auto paintw = width() - padding.left() - padding.right();

	const Part *parts[] = {
		_headerPart.get(),
		_optionsPart.get(),
		_addOptionPart.get(),
		_footerPart.get(),
	};
	auto tshift = 0;
	auto symbolAdd = uint16(0);
	for (const auto part : parts) {
		const auto h = part->countHeight(paintw);
		if (h > 0) {
			auto partResult = part->textState(
				point - QPoint(0, tshift),
				padding.left(),
				paintw,
				width(),
				request);
			if (partResult.link) {
				partResult.itemId = result.itemId;
				AddTextStateOffset(&partResult, symbolAdd);
				return partResult;
			}
			if (point.y() >= tshift && point.y() < tshift + h) {
				partResult.itemId = result.itemId;
				AddTextStateOffset(&partResult, symbolAdd);
				return partResult;
			}
		}
		symbolAdd += part->selectionLength();
		tshift += h;
	}
	return result;
}

void Poll::parentTextUpdated() {
	_headerPart->updateDescription();
	history()->owner().requestViewResize(_parent);
}

auto Poll::bubbleRoll() const -> BubbleRoll {
	const auto value = _wrongAnswerAnimation.value(1.);
	_wrongAnswerAnimated = (value < 1.);
	if (!_wrongAnswerAnimated) {
		return BubbleRoll();
	}
	const auto progress = [](float64 full) {
		const auto lower = std::floor(full);
		const auto shift = (full - lower);
		switch (int(lower) % 4) {
		case 0: return -shift;
		case 1: return (shift - 1.);
		case 2: return shift;
		case 3: return (1. - shift);
		}
		Unexpected("Value in Poll::getBubbleRollDegrees.");
	};
	return {
		.rotate = progress(value * kRotateSegments) * kRotateAmplitude,
		.scale = 1. + progress(value * kScaleSegments) * kScaleAmplitude
	};
}

QMargins Poll::bubbleRollRepaintMargins() const {
	if (!_wrongAnswerAnimated) {
		return QMargins();
	}
	return PollBubbleRollRepaintMargins(_parent->currentSize());
}

void Poll::paintBubbleFireworks(
		Painter &p,
		const QRect &bubble,
		crl::time ms) const {
	if (_lastDrawCanonical && _lastDrawPaintDevice == p.device()) {
		auto fireworks = QRegion();
		auto wrongAnswer = QRegion();
		auto known = true;
		if (!bubble.isEmpty()) {
			const auto mapped = mapCurrentPaintToElement(p, QRectF(bubble));
			if (!mapped) {
				known = false;
			} else if (!mapped->isEmpty()) {
				fireworks += *mapped;
				wrongAnswer += mapped->marginsAdded(
					PollBubbleRollRepaintMargins(bubble.size()));
			}
		}
		recordAnimationRepaintGeometry(
			_fireworksRepaint,
			std::move(fireworks),
			known);
		recordAnimationRepaintGeometry(
			_wrongAnswerRepaint,
			std::move(wrongAnswer),
			known);
	}
	if (!_wrongAnswerAnimation.animating()) {
		stopRepaintGeneration(_wrongAnswerRepaint);
	}
	if (!_fireworksAnimation) {
		stopRepaintGeneration(_fireworksRepaint);
		return;
	}
	if (_fireworksAnimation->paint(p, bubble)) {
		return;
	}
	_fireworksAnimation = nullptr;
	stopRepaintGeneration(_fireworksRepaint);
}

void Poll::clickHandlerActiveChanged(
		const ClickHandlerPtr &handler,
		bool active) {
	_headerPart->clickHandlerActiveChanged(handler, active);
}

void Poll::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (!handler) return;

	_headerPart->clickHandlerPressedChanged(handler, pressed);
	_optionsPart->clickHandlerPressedChanged(handler, pressed);
	_addOptionPart->clickHandlerPressedChanged(handler, pressed);
	_footerPart->clickHandlerPressedChanged(handler, pressed);
}

void Poll::hideSpoilers() {
	if (_headerPart->_description.hasSpoilers()) {
		_headerPart->_description.setSpoilerRevealed(
			false,
			anim::type::instant);
	}
	if (_headerPart->_solutionText.hasSpoilers()) {
		_headerPart->_solutionText.setSpoilerRevealed(
			false,
			anim::type::instant);
	}
}

void Poll::unloadHeavyPart() {
	_headerPart->unloadHeavyPart();
	_optionsPart->unloadHeavyPart();
}

bool Poll::hasHeavyPart() const {
	return _headerPart->hasHeavyPart()
		|| _optionsPart->hasHeavyPart();
}

void Poll::Options::toggleRipple(Answer &answer, bool pressed) {
	if (pressed) {
		const auto outerWidth = _owner->width();
		const auto innerWidth = outerWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		const auto maskSize = QSize(
			outerWidth,
			countAnswerHeight(answer, innerWidth));
		if (answer.ripple && answer.rippleMaskSize != maskSize) {
			resetAnswerRipple(answer);
		}
		if (!answer.ripple) {
			auto mask = Ui::RippleAnimation::RectMask(maskSize);
			const auto option = answer.option;
			const auto generation = resetFeedbackRepaint(
				option,
				FeedbackPart::Ripple);
			const auto weak = base::make_weak(_owner.get());
			answer.rippleMaskSize = maskSize;
			answer.ripple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				std::move(mask),
				[=] {
					if (const auto strong = weak.get()) {
						strong->_optionsPart->repaintAnswerFeedback(
							option,
							FeedbackPart::Ripple,
							generation);
					}
				});
		}
		auto answerTop = 0;
		for (const auto &a : _answers) {
			if (&a == &answer) {
				break;
			}
			answerTop += countAnswerHeight(a, innerWidth);
		}
		answer.ripple->add(_owner->_lastLinkPoint - QPoint(0, answerTop));
	} else if (answer.ripple) {
		answer.ripple->lastStop();
	}
}

bool Poll::Header::canShowSolution() const {
	return _owner->showVotes() && !_owner->_poll->solution.text.isEmpty();
}

bool Poll::Header::inShowSolution(
		QPoint point,
		int right,
		int top) const {
	if (!canShowSolution() || !_solutionButtonVisible) {
		return false;
	}
	const auto &icon = st::historyQuizExplainIn;
	const auto x = right - icon.width();
	const auto y = top + (st::normalFont->height - icon.height()) / 2;
	return style::rtlrect(
		x,
		y,
		icon.width(),
		icon.height(),
		_owner->width()).contains(point);
}

QString Poll::Footer::closeTimerText() const {
	if (_owner->_poll->closeDate <= 0
		|| (_owner->_flags & PollData::Flag::Closed)) {
		_closeTimer.cancel();
		return {};
	}
	const auto left = _owner->_poll->closeDate - base::unixtime::now();
	if (left <= 0) {
		_closeTimer.cancel();
		return {};
	}
	if (!_closeTimer.isActive()) {
		_closeTimer.callEach(1000);
	}
	const auto hideResults = (_owner->_flags
		& PollData::Flag::HideResultsUntilClose);
	if (left >= 86400) {
		const auto days = (left + 86399) / 86400;
		return hideResults
			? tr::lng_polls_results_in_days(tr::now, lt_count, days)
			: tr::lng_polls_ends_in_days(tr::now, lt_count, days);
	}
	const auto timer = Ui::FormatDurationText(left);
	return hideResults
		? tr::lng_polls_results_in_time(tr::now, lt_time, timer)
		: tr::lng_polls_ends_in_time(tr::now, lt_time, timer);
}

QRect Poll::Footer::timerRect(
		const Layout &layout,
		int left,
		int innerWidth) const {
	const auto timerText = closeTimerText();
	if (timerText.isEmpty()) {
		return {};
	}
	const auto lineHeight = st::msgDateFont->height;
	const auto timerw = st::msgDateFont->width(timerText);
	if (layout.timerSeparate) {
		return QRect(
			left + (innerWidth - timerw) / 2,
			layout.timerY,
			timerw,
			lineHeight);
	} else if (layout.timerFolded) {
		const auto sep = QString::fromUtf8(" \xC2\xB7 ");
		const auto label = _totalVotesLabel.toString();
		const auto prefixw = st::msgDateFont->width(label + sep);
		const auto fullw = prefixw + timerw;
		return QRect(
			left + (innerWidth - fullw) / 2 + prefixw,
			layout.textY,
			timerw,
			lineHeight);
	}
	return {};
}

bool Poll::Footer::timerFooterMultiline(int paintw) const {
	const auto timerText = closeTimerText();
	if (timerText.isEmpty()) {
		return false;
	}
	if (_owner->_voted && !_owner->showVotes()) {
		return true;
	}
	const auto sep = QString::fromUtf8(" \xC2\xB7 ");
	const auto full = _totalVotesLabel.toString()
		+ sep
		+ timerText;
	return centeredOverlapsInfo(st::msgDateFont->width(full), paintw);
}

bool Poll::Footer::centeredOverlapsInfo(
		int textWidth,
		int innerWidth) const {
	const auto skipw = _owner->_parent->skipBlockWidth();
	return (innerWidth + textWidth) / 2 > innerWidth - skipw;
}

Poll::~Poll() {
	for (const auto webpage : _registeredWebpages) {
		history()->owner().unregisterWebPageView(webpage, _parent);
	}
	history()->owner().unregisterPollView(_poll, _parent);
	if (hasHeavyPart()) {
		unloadHeavyPart();
		_parent->checkHeavyPart();
	}
}

void Poll::refreshWebpageSubscriptions() {
	auto wanted = std::vector<WebPageData*>();
	for (const auto &answer : _poll->answers) {
		if (const auto webpage = answer.media.webpage) {
			if (!ranges::contains(wanted, webpage)) {
				wanted.push_back(webpage);
			}
		}
	}
	for (const auto webpage : _registeredWebpages) {
		if (!ranges::contains(wanted, webpage)) {
			history()->owner().unregisterWebPageView(webpage, _parent);
		}
	}
	for (const auto webpage : wanted) {
		if (!ranges::contains(_registeredWebpages, webpage)) {
			history()->owner().registerWebPageView(webpage, _parent);
		}
	}
	_registeredWebpages = std::move(wanted);
}

} // namespace HistoryView
