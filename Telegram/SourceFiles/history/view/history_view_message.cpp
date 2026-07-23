/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_message.h"

#include "api/api_suggest_post.h"
#include "api/api_transcribes.h"
#include "base/options.h"
#include "base/qt/qt_key_modifiers.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/click_handler_types.h" // ClickHandlerContext
#include "core/ui_integration.h"
#include "history/view/history_view_cursor_state.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "history/view/media/history_view_media_generic.h"
#include "history/view/media/history_view_web_page.h"
#include "history/view/media/history_view_suggest_decision.h"
#include "history/view/reactions/history_view_reactions.h"
#include "history/view/reactions/history_view_reactions_button.h"
#include "history/view/history_view_reply_button.h"
#include "history/view/history_view_group_call_bar.h" // UserpicInRow.
#include "history/view/history_view_reply.h"
#include "history/view/history_view_transcribe_button.h"
#include "history/view/history_view_summary_header.h"
#include "history/view/history_view_view_button.h" // ViewButton.
#include "history/history.h"
#include "iv/markdown/iv_markdown_article_text.h"
#include "iv/markdown/iv_markdown_prepare_links.h"
#include "iv/iv_instance.h"
#include "iv/iv_rich_page.h"
#include "boxes/premium_preview_box.h"
#include "boxes/share_box.h"
#include "boxes/peers/tag_info_box.h"
#include "ui/effects/reaction_fly_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/round_checkbox.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/text/text_extended_data.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/damage_debug.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/rect.h"
#include "data/components/factchecks.h"
#include "data/components/sponsored_messages.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_forum_topic.h"
#include "data/data_message_reactions.h"
#include "lang/lang_keys.h"
#include "mainwidget.h"
#include "main/main_session.h"
#include "settings/sections/settings_premium.h"
#include "window/themes/window_theme.h" // IsNightMode.
#include "window/window_session_controller.h"
#include "apiwrap.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"
#include "styles/style_iv.h"
#include "styles/style_polls.h"

namespace HistoryView {
namespace {

base::options::toggle UnlimitedMessageWidth({
	.id = kOptionUnlimitedMessageWidth,
	.name = "Unlimited message width",
	.description = "Allow text-only message bubbles "
		"to expand beyond the default maximum width.",
	.restartRequired = true,
});

constexpr auto kPlayStatusLimit = 2;
constexpr auto kMaxWidth = (1 << 16) - 1;
constexpr auto kMaxNiceToReadLines = 6;
const auto kPsaTooltipPrefix = "cloud_lng_tooltip_psa_";
constexpr auto kFullLineAppearDuration = crl::time(300);
constexpr auto kFullLineAppearFinalDuration = crl::time(120);
constexpr auto kLineHeightAppearDuration = crl::time(100);
constexpr auto kLineHeightAppearFinalDuration = crl::time(60);
constexpr auto kMinWidthAppearDuration = crl::time(160);

[[nodiscard]] QRect MapRichPageRect(
		const HistoryMessageRichPage::RepaintGeometry &geometry,
		QRect rect) {
	if (!geometry.known
		|| geometry.articleSize.isEmpty()
		|| rect.isEmpty()) {
		return {};
	}
	const auto bounds = QRect(QPoint(), geometry.articleSize);
	const auto clipped = rect.intersected(bounds);
	return clipped.isEmpty()
		? QRect()
		: geometry.articleToElement.mapRect(QRectF(clipped)).toAlignedRect();
}

[[nodiscard]] std::optional<QTransform> RichPageArticleTransform(
		const Painter &p,
		const PaintContext &context,
		QPoint origin) {
	if (!context.elementTransform) {
		return std::nullopt;
	}
	auto invertible = false;
	const auto inverted = context.elementTransform->inverted(&invertible);
	if (!invertible) {
		return std::nullopt;
	}
	const auto result = QTransform::fromTranslate(origin.x(), origin.y())
		* p.transform()
		* inverted;
	return result.isInvertible()
		? std::optional<QTransform>(result)
		: std::nullopt;
}

[[nodiscard]] uint64 SyncRichPageRepaintGeneration(
		not_null<HistoryMessageRichPage*> rich,
		uint64 &ownerGeneration) {
	auto &geometry = rich->repaintGeometry;
	const auto page = rich->page.get();
	if (geometry.page == page) {
		return geometry.generation;
	}
	geometry.stale = geometry.stale.united(
		MapRichPageRect(geometry, geometry.pending));
	geometry.pending = QRect();
	geometry.known = false;
	geometry.page = page;
	if (!++ownerGeneration) {
		++ownerGeneration;
	}
	geometry.generation = ownerGeneration;
	return geometry.generation;
}

void InvalidateRichPageRepaintGeometry(
		not_null<HistoryMessageRichPage*> rich) {
	auto &geometry = rich->repaintGeometry;
	geometry.stale = geometry.stale.united(
		MapRichPageRect(geometry, geometry.pending));
	geometry.known = false;
}

[[nodiscard]] QSize TopicButtonSize(
		int availableWidth,
		const Ui::Text::String &name) {
	const auto padding = st::topicButtonPadding;
	const auto height = padding.top()
		+ st::msgNameFont->height
		+ padding.bottom();
	const auto width = std::max(
		std::min(
			availableWidth,
			padding.left()
				+ name.maxWidth()
				+ st::topicButtonArrowSkip
				+ padding.right()),
		height);
	return { width, height };
}

[[nodiscard]] int RevealLineRight(const Ui::Text::LineLayoutInfo &line) {
	return line.left + line.width;
}

using PreparedLink = Iv::Markdown::PreparedLink;
using PreparedLinkKind = Iv::Markdown::PreparedLinkKind;
using MediaActivation = Iv::Markdown::MediaActivation;
using MediaActivationKind = Iv::Markdown::MediaActivationKind;
using MarkdownArticleSelectionPosition = Iv::Markdown::MarkdownArticleSelectionPosition;
using MarkdownArticleSelection = Iv::Markdown::MarkdownArticleSelection;
using MarkdownArticleSelectionEndpoint = Iv::Markdown::MarkdownArticleSelectionEndpoint;
using MarkdownArticleSelectionEndpoints = Iv::Markdown::MarkdownArticleSelectionEndpoints;

[[nodiscard]] auto FlatSelectionEndpointFromState(const TextState &state)
-> MessageSelectionFlatEndpoint {
	return state.selectionCursor.isFlat()
		? state.selectionCursor.flat
		: MessageSelectionFlatEndpoint{
			.symbol = state.symbol,
			.afterSymbol = state.afterSymbol,
		};
}

void SetRichPageSelectionCursor(
		not_null<TextState*> state,
		int segment,
		int offset,
		bool direct) {
	state->selectionCursor = MessageSelectionEndpoint::RichPage(
		{
			.segment = segment,
			.offset = offset,
		},
		MarkdownArticleSelectionEndpoint{
			.segment = segment,
			.direct = direct,
		});
}

[[nodiscard]] MarkdownArticleSelection AdjustRichPageSelection(
		const Iv::Markdown::MarkdownArticle &article,
		MarkdownArticleSelectionPosition anchor,
		MarkdownArticleSelectionPosition focus,
		TextSelectType type) {
	if (anchor.segment == focus.segment) {
		const auto adjusted = article.adjustSelection(
			anchor.segment,
			TextSelection(
				uint16(std::min(anchor.offset, focus.offset)),
				uint16(std::max(anchor.offset, focus.offset))),
			type);
		return {
			.from = { .segment = anchor.segment, .offset = adjusted.from },
			.to = { .segment = anchor.segment, .offset = adjusted.to },
		};
	}
	const auto focusBeforeAnchor = CompareMessageSelectionPositions(
		focus,
		anchor) < 0;
	const auto anchorExpanded = article.adjustSelection(
		anchor.segment,
		TextSelection(uint16(anchor.offset), uint16(anchor.offset)),
		type);
	const auto focusExpanded = article.adjustSelection(
		focus.segment,
		TextSelection(uint16(focus.offset), uint16(focus.offset)),
		type);
	return {
		.from = {
			.segment = anchor.segment,
			.offset = focusBeforeAnchor
				? int(anchorExpanded.to)
				: int(anchorExpanded.from),
		},
		.to = {
			.segment = focus.segment,
			.offset = focusBeforeAnchor
				? int(focusExpanded.from)
				: int(focusExpanded.to),
		},
	};
}

[[nodiscard]] QString OpenableTargetForPreparedLink(const PreparedLink &link) {
	return link.fragment.isEmpty()
		? link.target
		: (link.target + u"#"_q + link.fragment);
}

[[nodiscard]] ClickHandler::TextEntity TextEntityForPreparedLink(
		const PreparedLink &link) {
	if (const auto external = ExternalEntityLinkData(link)) {
		return {
			.type = external->type,
			.data = external->data,
		};
	}
	return {};
}

[[nodiscard]] QString CopyTextForPreparedLink(const PreparedLink &link) {
	if (const auto external = ExternalEntityLinkData(link)) {
		return external->text;
	}
	switch (link.kind) {
	case PreparedLinkKind::Anchor:
	case PreparedLinkKind::FootnoteBacklink:
		return link.target.isEmpty() ? QString() : (u"#"_q + link.target);
	case PreparedLinkKind::Footnote:
		return !link.copyText.isEmpty() ? link.copyText : link.target;
	case PreparedLinkKind::LocalFile:
	case PreparedLinkKind::InstantViewPage:
		return OpenableTargetForPreparedLink(link);
	case PreparedLinkKind::External:
		return link.target;
	case PreparedLinkKind::RejectedRelative:
	case PreparedLinkKind::ToggleDetails:
		return QString();
	}
	return QString();
}

[[nodiscard]] QString CopyLabelForPreparedLink(const PreparedLink &link) {
	if (const auto external = ExternalEntityLinkData(link)) {
		return (external->type == EntityType::Email)
			? Ui::Integration::Instance().phraseContextCopyEmail()
			: Ui::Integration::Instance().phraseContextCopyLink();
	}
	switch (link.kind) {
	case PreparedLinkKind::RejectedRelative:
	case PreparedLinkKind::ToggleDetails:
		return QString();
	case PreparedLinkKind::External:
	case PreparedLinkKind::InstantViewPage:
	case PreparedLinkKind::Anchor:
	case PreparedLinkKind::Footnote:
	case PreparedLinkKind::FootnoteBacklink:
	case PreparedLinkKind::LocalFile:
		return tr::lng_context_copy_link(tr::now);
	}
	return QString();
}

void CopyRichPageCodeBlockText(TextForMimeData text, ClickContext context) {
	if (context.button != Qt::LeftButton || text.empty()) {
		return;
	} else if (!text.rich.text.endsWith('\n')) {
		text.rich.text.append('\n');
	}
	if (!text.expanded.endsWith('\n')) {
		text.expanded.append('\n');
	}
	if (Ui::Integration::Instance().copyPreOnClick(context.other)) {
		TextUtilities::SetClipboardText(std::move(text));
	}
}

[[nodiscard]] bool SamePreparedLink(
		const std::optional<PreparedLink> &a,
		const std::optional<PreparedLink> &b) {
	if (!a || !b) {
		return !a && !b;
	}
	return (a->index == b->index)
		&& (a->kind == b->kind)
		&& (a->target == b->target)
		&& (a->fragment == b->fragment)
		&& (a->copyText == b->copyText)
		&& (a->entityType == b->entityType)
		&& (a->shown == b->shown)
		&& (a->webpageId == b->webpageId);
}

[[nodiscard]] bool SameEmbedRequest(
		const Iv::Markdown::EmbedRequest &a,
		const Iv::Markdown::EmbedRequest &b) {
	return (a.html == b.html)
		&& (a.url == b.url)
		&& (a.width == b.width)
		&& (a.height == b.height)
		&& (a.fullWidth == b.fullWidth)
		&& (a.fixedHeight == b.fixedHeight)
		&& (a.allowScrolling == b.allowScrolling);
}

[[nodiscard]] bool SameMediaActivation(
		const MediaActivation &a,
		const MediaActivation &b) {
	return (a.kind == b.kind)
		&& (a.url == b.url)
		&& SameEmbedRequest(a.embed, b.embed)
		&& (a.placeholderId.value == b.placeholderId.value)
		&& (a.photo.get() == b.photo.get())
		&& (a.document.get() == b.document.get())
		&& (a.channel.get() == b.channel.get());
}

class RichPageActionClickHandler final : public ClickHandler {
public:
	RichPageActionClickHandler(
		Fn<void(ClickContext)> callback,
		std::optional<PreparedLink> link = std::nullopt)
	: _callback(std::move(callback))
	, _link(std::move(link)) {
	}

	void onClick(ClickContext context) const override {
		if (_callback) {
			_callback(std::move(context));
		}
	}

	QString url() const override {
		return _link ? _link->target : QString();
	}

	QString copyToClipboardText() const override {
		return _link ? CopyTextForPreparedLink(*_link) : QString();
	}

	QString copyToClipboardContextItemText() const override {
		return _link ? CopyLabelForPreparedLink(*_link) : QString();
	}

	TextEntity getTextEntity() const override {
		return _link ? TextEntityForPreparedLink(*_link) : TextEntity();
	}

	QString tooltip() const override {
		return _link
			? Iv::Markdown::TooltipForPreparedLink(*_link)
			: QString();
	}

private:
	const Fn<void(ClickContext)> _callback;
	const std::optional<PreparedLink> _link;
};

void ApplyRevealGradient(
		not_null<const TextAppearing*> appearing,
		QImage &cache,
		int availableWidth) {
	const auto ratio = style::DevicePixelRatio();
	const auto maskWidth = int(st::textRevealGradient) * ratio;
	if (appearing->gradientMask.width() != maskWidth) {
		auto mask = QImage(
			QSize(maskWidth, 1),
			QImage::Format_ARGB32_Premultiplied);
		mask.setDevicePixelRatio(ratio);
		mask.fill(Qt::transparent);
		{
			const auto logicalWidth = int(st::textRevealGradient);
			auto p = QPainter(&mask);
			auto gradient = QLinearGradient(0, 0, logicalWidth, 0);
			gradient.setStops({
				{ 0., QColor(255, 255, 255, 255) },
				{ 1., QColor(255, 255, 255, 0) },
			});
			p.fillRect(0, 0, logicalWidth, 1, gradient);
		}
		appearing->gradientMask = std::move(mask);
	}

	const auto cacheW = int(cache.width() / cache.devicePixelRatio());
	const auto cacheH = int(cache.height() / cache.devicePixelRatio());
	const auto revealedWidth = appearing->revealedLineWidth;
	const auto gradientWidth = std::min(
		int(st::textRevealGradient),
		revealedWidth);
	const auto lineRtl = appearing->lines[appearing->shownLine].rtl;

	auto p = QPainter(&cache);
	p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
	if (lineRtl) {
		const auto clampedAvailableWidth = std::min(
			std::max(availableWidth, 0),
			cacheW);
		const auto rightEdge = clampedAvailableWidth - revealedWidth;
		p.fillRect(
			QRect(0, 0, rightEdge, cacheH),
			Qt::transparent);
		p.save();
		p.translate(rightEdge + gradientWidth, 0);
		p.scale(-1, 1);
		p.drawImage(
			QRect(0, 0, gradientWidth, cacheH),
			appearing->gradientMask);
		p.restore();
	} else {
		p.fillRect(
			QRect(revealedWidth, 0, cacheW - revealedWidth, cacheH),
			Qt::transparent);
		p.drawImage(
			QRect(revealedWidth - gradientWidth, 0, gradientWidth, cacheH),
			appearing->gradientMask);
	}
}

struct SecondRightAction {
	std::unique_ptr<Ui::RippleAnimation> ripple;
	ClickHandlerPtr link;
};

struct BadgePillGeometry {
	int textWidth = 0;
	int width = 0;
	int height = 0;
};

[[nodiscard]] bool IsRippleLink(const ClickHandlerPtr &handler) {
	switch (handler->getTextEntity().type) {
	case EntityType::Url:
	case EntityType::CustomUrl:
	case EntityType::Email:
	case EntityType::Hashtag:
	case EntityType::Cashtag:
	case EntityType::Mention:
	case EntityType::MentionName:
	case EntityType::BotCommand:
	case EntityType::Phone:
	case EntityType::BankCard:
	case EntityType::FormattedDate:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] ClickHandlerPtr MakeTopicButtonLink(
		not_null<Data::ForumTopic*> topic,
		MsgId messageId) {
	const auto weak = base::make_weak(topic);
	return std::make_shared<LambdaClickHandler>([=](ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		if (const auto controller = my.sessionWindow.get()) {
			if (const auto strong = weak.get()) {
				controller->showTopic(
					strong,
					messageId,
					Window::SectionShow::Way::Forward);
			}
		}
	});
}

[[nodiscard]] BadgePillGeometry ComputeBadgePillGeometry(
		not_null<const RightBadge*> badge) {
	const auto &padding = st::msgTagBadgePadding;
	const auto textWidth = badge->tag.maxWidth();
	const auto contentWidth = padding.left()
		+ textWidth
		+ padding.right();
	const auto height = padding.top()
		+ st::msgFont->height
		+ padding.bottom();
	return {
		.textWidth = textWidth,
		.width = std::max(contentWidth, height),
		.height = height,
	};
}

struct PaintedRectRepaint {
	QRect current;
	QRect stale;
	uint32 pending : 1 = 0;
	uint32 known : 1 = 0;
};

void RequestPaintedRectRepaint(
		not_null<const Element*> view,
		PaintedRectRepaint &repaint,
		const char *component) {
	if (repaint.pending || (repaint.known && repaint.current.isEmpty())) {
		return;
	}
	repaint.pending = 1;
	if (!repaint.known) {
		Ui::LogUnknownGeometryRepaint(component);
		view->repaint();
	} else {
		view->repaint(repaint.current);
	}
}

void RecordPaintedRectRepaint(
		not_null<const Element*> view,
		PaintedRectRepaint &repaint,
		bool canonical,
		std::optional<QRect> painted,
		bool skipConvergence,
		const char *component) {
	if (!canonical) {
		return;
	}
	const auto stale = base::take(repaint.stale);
	const auto previous = stale.united(base::take(repaint.current));
	repaint.pending = 0;
	repaint.current = painted.value_or(QRect());
	repaint.known = painted.has_value() ? 1 : 0;
	if (skipConvergence) {
		return;
	} else if (!repaint.known) {
		repaint.stale = previous;
		if (!previous.isEmpty()) {
			repaint.pending = 1;
			Ui::LogUnknownGeometryRepaint(component);
			view->repaint();
		}
	} else if (!previous.isEmpty()
		&& (!stale.isEmpty() || previous != repaint.current)) {
		repaint.pending = 1;
		view->repaint(previous.united(repaint.current));
	}
}

} // namespace

const char kOptionUnlimitedMessageWidth[]
	= "unlimited-message-width";

struct Message::CommentsButton {
	std::unique_ptr<Ui::RippleAnimation> ripple;
	std::vector<UserpicInRow> userpics;
	QImage cachedUserpics;
	ClickHandlerPtr link;
	QPoint lastPoint;
	int rippleShift = 0;
};

struct Message::FromNameStatus {
	EmojiStatusId id;
	std::unique_ptr<Ui::Text::CustomEmoji> custom;
	ClickHandlerPtr link;
	std::optional<QRect> lastRepaintRect;
	int skip = 0;
};

struct Message::SelectionCheckbox {
	explicit SelectionCheckbox(Fn<void()> repaint)
	: check(st::msgSelectionCheck, std::move(repaint)) {
	}

	Ui::RoundCheckbox check;
	PaintedRectRepaint repaint;
	bool moving = false;
};

struct Message::RightAction {
	std::unique_ptr<Ui::RippleAnimation> ripple;
	ClickHandlerPtr link;
	QPoint lastPoint;
	std::unique_ptr<SecondRightAction> second;
};

struct Message::LinkRipple {
	std::unique_ptr<Ui::RippleAnimation> ripple;
	ClickHandlerPtr link;
	QPoint maskOffset;
	QRect current;
	QRect stale;
	QSize maskSize;
	uint64 generation = 0;
	int cachedWidth = 0;
	bool pending = false;
	bool known = false;
};

LogEntryOriginal::LogEntryOriginal() = default;

LogEntryOriginal::LogEntryOriginal(LogEntryOriginal &&other)
: page(std::move(other.page)) {
}

LogEntryOriginal &LogEntryOriginal::operator=(LogEntryOriginal &&other) {
	page = std::move(other.page);
	return *this;
}

LogEntryOriginal::~LogEntryOriginal() = default;

Message::Message(
	not_null<ElementDelegate*> delegate,
	not_null<HistoryItem*> data,
	Element *replacing)
: Element(delegate, data, replacing, Flag(0))
, _hideReply(delegate->elementHideReply(this))
, _postShowingAuthor(data->isPostShowingAuthor() ? 1 : 0)
, _bottomInfo(
		&data->history()->owner().reactions(),
		BottomInfoDataFromMessage(this)) {
	if (data->Get<HistoryMessageSuggestion>()) {
		_hideReply = 1;
	} else if (const auto media = data->media()) {
		if (media->giveawayResults()) {
			_hideReply = 1;
		}
	}
	initLogEntryOriginal();
	initPsa();
	if (data->displayHiddenSenderInfo()) {
		AddComponents(HiddenSenderTooltip::Bit());
	}
	setupReactions(replacing);
	auto animation = replacing ? replacing->takeEffectAnimation() : nullptr;
	if (animation) {
		_bottomInfo.continueEffectAnimation(std::move(animation));
	}
	if (data->isSponsored()) {
		const auto &session = data->history()->session();
		const auto details = session.sponsoredMessages().lookupDetails(
			data->fullId());
		if (details.canReport) {
			_rightAction = std::make_unique<RightAction>();
			_rightAction->second = std::make_unique<SecondRightAction>();

			_rightAction->second->link = ReportSponsoredClickHandler(data);
		}
	}
	initPaidInformation();

	if (data->textAppearing()) {
		AddComponents(TextAppearing::Bit());
		const auto appearing = Get<TextAppearing>();
		if (replacing) {
			if (const auto was = replacing->Get<TextAppearing>()) {
				*appearing = std::move(*was);
				appearing->widthAnimation.setCallback([=] {
					textAppearWidthCallback();
				});
				appearing->heightAnimation.setCallback([=] {
					textAppearHeightCallback();
				});
			}
		}
		if (data->textAppearingStarted()
			&& !appearing->widthAnimation.animating()
			&& !appearing->heightAnimation.animating()) {
			skipInactiveTextAppearing();
		}
	}
}

Message::~Message() {
	resetTopicButton();
	if (_comments || (_fromNameStatus && _fromNameStatus->custom)) {
		_comments = nullptr;
		_fromNameStatus = nullptr;
		checkHeavyPart();
	}
}

void HistoryMessageRichPage::Host::requestRepaint(QRect articleRect) {
	if (const auto strong = owner.get()) {
		const auto rich = strong->richpage();
		if (!rich || rich->host.get() != this) {
			return;
		}
		crl::on_main(strong, [
				weak = owner,
				alive = std::weak_ptr<bool>(lifetime),
				host = this,
				page = rich->page,
				articleRect] {
			const auto owner = weak.get();
			const auto rich = owner ? owner->richpage() : nullptr;
			if (!alive.expired()
				&& rich
				&& rich->host.get() == host
				&& rich->page == page) {
				owner->requestRichPageRepaint(articleRect);
			}
		});
	}
}

void HistoryMessageRichPage::Host::requestRelayout(QRect articleRect) {
	if (const auto strong = owner.get()) {
		const auto rich = strong->richpage();
		if (!rich || rich->host.get() != this) {
			return;
		}
		crl::on_main(strong, [
				weak = owner,
				alive = std::weak_ptr<bool>(lifetime),
				host = this,
				page = rich->page,
				articleRect] {
			const auto owner = weak.get();
			const auto rich = owner ? owner->richpage() : nullptr;
			if (!alive.expired()
				&& rich
				&& rich->host.get() == host
				&& rich->page == page) {
				owner->requestRichPageRelayout(articleRect);
			}
		});
	}
}

HistoryMessageRichPage::HistoryMessageRichPage()
: host(std::make_unique<Host>())
, article(st::messageMarkdown) {
}

void Message::setInstantViewMediaRuntime(QString pageUrl) {
	Expects(Has<InstantViewMediaRuntime>()
		|| !Has<HistoryMessageRichPage>());
	// We mustn't add the component here if we're in rich message,
	// because we call this method where we remember RichPage reference.

	AddComponents(InstantViewMediaRuntime::Bit());
	Get<InstantViewMediaRuntime>()->pageUrl = std::move(pageUrl);
}

bool Message::hasRichPage() const {
	return (richpage() != nullptr);
}

void Message::requestRichPageRepaint(QRect articleRect) const {
	requestRichPageRepaint(articleRect, 0);
}

void Message::requestRichPageRepaint(
		QRect articleRect,
		uint64 generation) const {
	const auto rich = const_cast<Message*>(this)->richpage();
	if (!rich || isHidden()) {
		return;
	}
	const auto currentGeneration = SyncRichPageRepaintGeneration(
		rich,
		_richPageRepaintGeneration);
	if (generation && generation != currentGeneration) {
		return;
	} else if (articleRect.isEmpty()) {
		Ui::LogUnknownGeometryRepaint("rich page");
		repaint();
		return;
	}
	auto &geometry = rich->repaintGeometry;
	if (geometry.known
		&& geometry.layoutWidth != rich->article.lastLayoutWidth()) {
		InvalidateRichPageRepaintGeometry(rich);
	}
	if (!geometry.known) {
		geometry.pending = geometry.pending.united(articleRect);
		Ui::LogUnknownGeometryRepaint("rich page");
		repaint();
		return;
	}
	const auto clipped = articleRect.intersected(
		QRect(QPoint(), geometry.articleSize));
	if (clipped.isEmpty()) {
		return;
	}
	geometry.pending = geometry.pending.united(clipped);
	const auto mapped = MapRichPageRect(geometry, clipped);
	if (mapped.isEmpty()) {
		Ui::LogUnknownGeometryRepaint("rich page");
		repaint();
		return;
	}
	repaint(mapped);
}

void Message::requestRichPageRelayout(QRect articleRect) {
	Q_UNUSED(articleRect);
	if (const auto rich = const_cast<Message*>(this)->richpage()) {
		InvalidateRichPageRepaintGeometry(rich);
		rich->article.invalidateLayout();
	}
	// The article layout was just invalidated, but textHeightFor() caches by
	// _textWidth and would short-circuit when re-queried at the same (constant)
	// max width, leaving the bubble sized from a stale, now-zeroed
	// lastLayoutWidth(). Drop the text-size cache too so the article is really
	// laid out again, the same way blockquoteExpandChanged() does for quotes.
	invalidateTextSizeCache();
	setPendingResize();
	history()->owner().requestViewResize(this);
}

void Message::activateRichPagePreparedLink(
		const PreparedLink &link,
		ClickContext context) const {
	if (context.button != Qt::LeftButton
		&& context.button != Qt::MiddleButton) {
		return;
	}
	const auto resolveInlineAnchor = [owner = const_cast<Message*>(this)](
			const QString &anchorId) {
		if (anchorId.isEmpty()) {
			return false;
		}
		const auto rich = owner->richpage();
		if (!rich) {
			return false;
		}
		auto top = rich->article.anchorTop(anchorId);
		auto trect = QRect();
		auto haveTrect = false;
		if (top < 0) {
			const auto expansion = rich->article.expandDetailsToAnchor(anchorId);
			if (expansion.changed) {
				haveTrect = owner->prepareRichPageTextRect(trect);
				if (!haveTrect) {
					return false;
				}
				const auto rect = owner->richPageRect(trect);
				static_cast<void>(rich->article.resizeGetHeight(rect.width()));
				top = rich->article.anchorTop(anchorId);
				owner->requestRichPageRelayout(QRect());
			} else {
				top = rich->article.anchorTop(anchorId);
			}
			if (top < 0) {
				return false;
			}
		}
		if (!haveTrect && !owner->prepareRichPageTextRect(trect)) {
			return false;
		}
		return owner->delegate()->elementScrollToLocalY(
			owner,
			owner->richPageRect(trect).top() + top);
	};
	switch (link.kind) {
	case PreparedLinkKind::External:
		if (const auto data = ExternalEntityLinkData(link)) {
			if (const auto handler = Ui::Integration::Instance()
					.createLinkHandler(*data, Ui::Text::MarkedContext())) {
				handler->onClick(std::move(context));
			}
		}
		break;
	case PreparedLinkKind::InstantViewPage: {
		const auto target = OpenableTargetForPreparedLink(link);
		if (target.isEmpty()) {
			return;
		}
		if (const auto controller = ExtractController(context)) {
			Core::App().iv().openWithIvPreferred(
				controller,
				target,
				context.other);
		} else {
			UrlClickHandler::Open(target, context.other);
		}
	} break;
	case PreparedLinkKind::Anchor:
	case PreparedLinkKind::Footnote:
	case PreparedLinkKind::FootnoteBacklink: {
		if (resolveInlineAnchor(link.target)) {
			break;
		}
		const auto rich = richpage();
		if (rich && rich->page && rich->page->part) {
			if (const auto controller = ExtractController(context)) {
				Core::App().iv().showRichMessage(
					controller,
					data(),
					link.target);
			}
		} else if (const auto controller = ExtractController(context)) {
			controller->showToast(
				tr::lng_iv_not_found_in_message(tr::now));
		}
		break;
	}
	case PreparedLinkKind::LocalFile: {
		const auto target = OpenableTargetForPreparedLink(link);
		if (!target.isEmpty()) {
			UrlClickHandler::Open(target, context.other);
		}
	} break;
	case PreparedLinkKind::RejectedRelative:
		break;
	case PreparedLinkKind::ToggleDetails:
		if (const auto rich = const_cast<Message*>(this)->richpage()
			; rich && rich->article.toggleDetails(link.target)) {
			const_cast<Message*>(this)->requestRichPageRelayout(QRect());
		}
		break;
	}
}

QRect Message::richPageRect(QRect trect) const {
	trect.setTop(trect.top() + st::mediaInBubbleSkip);
	return trect.marginsAdded(
		{ st::msgPadding.left(), 0, st::msgPadding.right(), 0 });
}

bool Message::prepareRichPageTextRect(QRect &trect) const {
	if (!hasVisibleText()) {
		return false;
	}
	const auto item = data();
	const auto media = this->media();
	auto g = countGeometry();
	if (g.width() < 1 || isHidden() || !drawBubble()) {
		return false;
	}
	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	const auto mediaDisplayed = media && media->isDisplayed();
	if (_reactions && !reactionsInBubble) {
		g.setHeight(g.height() - st::mediaInBubbleSkip - _reactions->height());
	}
	if (const auto keyboard = item->inlineReplyKeyboard()) {
		g.setHeight(
			g.height()
			- st::msgBotKbButton.margin
			- keyboard->naturalHeight());
	}
	auto inner = g;
	if (_comments) {
		inner.setHeight(inner.height() - st::historyCommentsButtonHeight);
	}
	trect = inner.marginsRemoved(st::msgPadding);
	const auto additionalInfoSkip = (mediaDisplayed
		&& !media->additionalInfoString().isEmpty())
		? st::msgDateFont->height
		: 0;
	const auto reactionsTop = (reactionsInBubble && !_viewButton)
		? (additionalInfoSkip + st::mediaInBubbleSkip)
		: additionalInfoSkip;
	const auto reactionsHeight = reactionsInBubble
		? (reactionsTop + _reactions->height())
		: 0;
	if (reactionsInBubble) {
		trect.setHeight(trect.height() - reactionsHeight);
	}
	if (_viewButton) {
		trect.setHeight(trect.height() - _viewButton->height());
		if (reactionsInBubble) {
			trect.setHeight(
				trect.height()
				- st::mediaInBubbleSkip
				+ st::msgPadding.bottom());
		} else if (mediaDisplayed) {
			trect.setHeight(trect.height() - st::mediaInBubbleSkip);
		}
	}
	const auto check = factcheckBlock();
	const auto entry = logEntryOriginal();
	const auto mediaOnBottom = (mediaDisplayed && media->isBubbleBottom())
		|| check
		|| entry;
	const auto mediaOnTop = (mediaDisplayed && media->isBubbleTop())
		|| (entry && entry->isBubbleTop());
	if (mediaOnBottom) {
		trect.setHeight(trect.height() + st::msgPadding.bottom());
	}
	if (mediaOnTop) {
		trect.setY(trect.y() - st::msgPadding.top());
	} else {
		if (displayFromName()) {
			trect.setTop(trect.top() + st::msgNameFont->height);
		}
		if (const auto badge = Get<EphemeralBadge>()) {
			trect.setTop(trect.top() + badge->height);
		}
		if (displayedTopicButton()) {
			trect.setTop(trect.top()
				+ st::topicButtonSkip
				+ st::topicButtonPadding.top()
				+ st::msgNameFont->height
				+ st::topicButtonPadding.bottom()
				+ st::topicButtonSkip);
		}
		if (displayForwardedFrom()) {
			const auto forwarded = item->Get<HistoryMessageForwarded>();
			const auto skip = forwarded->psaType.isEmpty()
				? 0
				: st::historyPsaIconSkip1;
			const auto fits = (forwarded->text.maxWidth()
				<= (trect.width() - skip));
			trect.setTop(trect.top()
				+ ((fits ? 1 : 2) * st::semiboldFont->height));
		}
		if (item->Has<HistoryMessageVia>()
			&& !displayFromName()
			&& !displayForwardedFrom()) {
			trect.setTop(trect.top() + st::msgNameFont->height);
		}
		if (const auto reply = Get<Reply>()) {
			trect.setTop(trect.top() + reply->height());
		}
		if (const auto summaryHeader = Get<SummaryHeader>()) {
			trect.setTop(trect.top() + summaryHeader->height());
		}
	}
	if (entry) {
		trect.setHeight(trect.height() - entry->height());
	}
	if (check) {
		trect.setHeight(
			trect.height()
			- check->height()
			- st::mediaInBubbleSkip);
	}
	if (mediaDisplayed && _invertMedia) {
		const auto mediaTop = trect.y()
			+ (mediaOnTop ? 0 : st::mediaInBubbleSkip);
		trect.setY(mediaTop
			+ media->height()
			+ (mediaOnBottom ? 0 : st::mediaInBubbleSkip));
	}
	return true;
}

QPoint Message::prepareRichPageStateRect(QPoint point, QRect &trect) const {
	if (const auto botTop = Get<FakeBotAboutTop>()) {
		trect.setY(trect.y() + botTop->height);
	}
	trect = richPageRect(trect);
	return point - trect.topLeft();
}

void Message::activateRichPageMedia(
		const MediaActivation &activation,
		ClickContext context) const {
	if (context.button != Qt::LeftButton
		&& context.button != Qt::MiddleButton) {
		return;
	}
	switch (activation.kind) {
	case MediaActivationKind::None:
		break;
	case MediaActivationKind::Embed:
		if (!activation.embed.url.isEmpty()) {
			HiddenUrlClickHandler::Open(
				activation.embed.url,
				context.other);
		}
		break;
	case MediaActivationKind::ExternalUrl:
		if (!activation.url.isEmpty()) {
			HiddenUrlClickHandler::Open(activation.url, context.other);
		}
		break;
	case MediaActivationKind::Photo:
		if (activation.photo) {
			activation.photo->open(context.button);
		}
		break;
	case MediaActivationKind::Document:
		if (activation.document) {
			activation.document->open(context.button);
		}
		break;
	case MediaActivationKind::OpenChannel:
		if (activation.channel) {
			activation.channel->open(context.button);
		}
		break;
	case MediaActivationKind::JoinChannel:
		if (activation.channel) {
			activation.channel->join(context.button);
		}
		break;
	}
}

void Message::refreshSuggestedInfo(
		not_null<HistoryItem*> item,
		not_null<const HistoryMessageSuggestion*> suggest,
		const HistoryMessageReply *replyData) {
	const auto link = (replyData && replyData->resolvedMessage)
		? JumpToMessageClickHandler(
			replyData->resolvedMessage.get(),
			item->fullId())
		: ClickHandlerPtr();
	setServicePreMessage({}, link, std::make_unique<MediaGeneric>(
		this,
		GenerateSuggestRequestMedia(this, suggest),
		MediaGenericDescriptor{
			.maxWidth = st::chatSuggestWidth,
			.fullAreaLink = link,
			.service = true,
			.hideServiceText = true,
		}));
}

void Message::initPaidInformation() {
	const auto item = data();
	if (item->history()->peer->isMonoforum()) {
		if (const auto suggest = item->Get<HistoryMessageSuggestion>()) {
			const auto replyData = item->Get<HistoryMessageReply>();
			refreshSuggestedInfo(item, suggest, replyData);
		}
		return;
	} else if (!item->history()->peer->isUser()) {
		return;
	}
	const auto media = this->media();
	const auto mine = PaidInformation{
		.messages = 1,
		.stars = item->starsPaid(),
	};
	auto info = media ? media->paidInformation().value_or(mine) : mine;
	if (!info) {
		return;
	}
	const auto action = [&] {
		return (info.messages == 1)
			? tr::lng_action_paid_message_one(
				tr::now,
				tr::marked)
			: tr::lng_action_paid_message_some(
				tr::now,
				lt_count,
				info.messages,
				tr::marked);
	};
	auto text = PreparedServiceText{
		.text = item->out()
			? tr::lng_action_paid_message_sent(
				tr::now,
				lt_count,
				info.stars,
				lt_action,
				action(),
				tr::marked)
			: tr::lng_action_paid_message_got(
				tr::now,
				lt_count,
				info.stars,
				lt_name,
				tr::link(item->from()->shortName(), 1),
				tr::marked),
	};
	if (!item->out()) {
		text.links.push_back(item ->from()->createOpenLink());
	}
	setServicePreMessage(std::move(text));
}

void Message::refreshRightBadge() {
	if (const auto badge = Get<RightBadge>(); badge && badge->overridden) {
		return;
	}
	if (hasOutLayout()) {
		if (Has<RightBadge>()) {
			RemoveComponents(RightBadge::Bit());
		}
		return;
	}
	const auto item = data();
	const auto [text, role, special] = [&]() -> std::tuple<QString, BadgeRole, bool> {
		if (item->isDiscussionPost()) {
			return {
				(delegate()->elementContext() == Context::Replies)
					? QString()
					: tr::lng_channel_badge(tr::now),
				BadgeRole::User,
				true,
			};
		} else if (item->author()->isMegagroup()) {
			if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
				if (!msgsigned->viaBusinessBot) {
					Assert(msgsigned->isAnonymousRank);
					return { msgsigned->author, BadgeRole::User, false };
				}
			}
		}
		const auto channel = item->history()->peer->asMegagroup();
		const auto user = item->author()->asUser();
		if (!channel) {
			if (const auto chat = item->history()->peer->asChat()) {
				if (user) {
					const auto j = chat->memberRanks.find(
						peerToUser(user->id));
					if (j != chat->memberRanks.end()) {
						const auto basicRole
							= (peerToUser(user->id) == chat->creator)
							? BadgeRole::Creator
							: chat->admins.contains(user)
							? BadgeRole::Admin
							: BadgeRole::User;
						return { j->second, basicRole, false };
					}
				}
			}
			return { QString(), BadgeRole::User, false };
		}
		if (!user) {
			return { QString(), BadgeRole::User, false };
		}
		const auto info = channel->mgInfo.get();
		const auto userId = peerToUser(user->id);
		const auto isCreator = (info->creator == user);
		const auto isAdmin = info->admins.contains(userId);
		if (isCreator || isAdmin) {
			const auto r = info->memberRanks.find(userId);
			if (r != info->memberRanks.end() && !r->second.isEmpty()) {
				return {
					r->second,
					isCreator ? BadgeRole::Creator : BadgeRole::Admin,
					false,
				};
			}
			if (isCreator) {
				return { tr::lng_owner_badge(tr::now), BadgeRole::Creator, false };
			}
			return { tr::lng_admin_badge(tr::now), BadgeRole::Admin, false };
		}
		const auto fromRank = item->fromRank();
		if (!fromRank.isEmpty()) {
			return { fromRank, BadgeRole::User, false };
		}
		return { QString(), BadgeRole::User, false };
	}();
	auto tagText = TextWithEntities{
		(text.isEmpty()
			? delegate()->elementAuthorRank(this)
			: TextUtilities::RemoveEmoji(TextUtilities::SingleLine(text)))
	};
	const auto boosts = item->boostsApplied();
	const auto needBadge = !tagText.empty() || boosts;
	if (!needBadge) {
		if (Has<RightBadge>()) {
			RemoveComponents(RightBadge::Bit());
		}
		return;
	}
	if (!Has<RightBadge>()) {
		AddComponents(RightBadge::Bit());
	}
	const auto badge = Get<RightBadge>();
	badge->role = role;
	badge->special = special || (text.isEmpty() && !tagText.empty());
	badge->tagLink = nullptr;
	badge->ripple = nullptr;
	if (tagText.empty()) {
		badge->tag.clear();
	} else {
		badge->tag.setMarkedText(
			st::defaultTextStyle,
			tagText,
			Ui::NameTextOptions());
	}
	if (boosts) {
		const auto many = (boosts > 1);
		auto boostText = Ui::Text::IconEmoji(many
			? &st::boostsMessageIcon
			: &st::boostMessageIcon
		).append(many ? QString::number(boosts) : QString());
		badge->boosts.setMarkedText(
			st::defaultTextStyle,
			boostText,
			Ui::NameTextOptions());
	} else {
		badge->boosts.clear();
	}
	const auto boostWidth = badge->boosts.isEmpty()
		? 0
		: (st::msgTagBadgeBoostSkip + badge->boosts.maxWidth());
	if (badge->role == BadgeRole::User) {
		const auto tagWidth = badge->tag.isEmpty()
			? 0
			: badge->tag.maxWidth();
		badge->width = tagWidth + boostWidth;
	} else {
		const auto &padding = st::msgTagBadgePadding;
		const auto textWidth = badge->tag.maxWidth();
		const auto contentWidth = padding.left()
			+ textWidth
			+ padding.right();
		const auto pillHeight = padding.top()
			+ st::msgFont->height
			+ padding.bottom();
		badge->width = std::max(contentWidth, pillHeight) + boostWidth;
	}
}

int Message::rightBadgeWidth() const {
	const auto badge = Get<RightBadge>();
	return badge ? badge->width : 0;
}

void Message::applyGroupAdminChanges(
		const base::flat_set<UserId> &changes) {
	if (!data()->out()
		&& changes.contains(peerToUser(data()->author()->id))) {
		history()->owner().requestViewResize(this);
	}
}

void Message::animateReaction(Ui::ReactionFlyAnimationArgs &&args) {
	const auto item = data();
	const auto media = this->media();

	auto g = countGeometry();
	if (g.width() < 1 || isHidden()) {
		return;
	}
	const auto bubble = drawBubble();
	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	const auto mediaDisplayed = media && media->isDisplayed();

	if (_reactions && !reactionsInBubble) {
		const auto reactionsHeight = st::mediaInBubbleSkip + _reactions->height();
		const auto reactionsLeft = (!bubble && mediaDisplayed)
			? media->contentRectForReactions().x()
			: 0;
		g.setHeight(g.height() - reactionsHeight);
		const auto reactionsPosition = QPoint(reactionsLeft + g.left(), g.top() + g.height() + st::mediaInBubbleSkip);
		_reactions->animate(args.translated(-reactionsPosition));
		return;
	}

	const auto keyboard = item->inlineReplyKeyboard();
	auto keyboardHeight = 0;
	if (keyboard) {
		keyboardHeight = keyboard->naturalHeight();
		g.setHeight(g.height() - st::msgBotKbButton.margin - keyboardHeight);
	}

	if (bubble) {
		// Entry page is always a bubble bottom.
		auto inner = g;
		if (_comments) {
			inner.setHeight(inner.height() - st::historyCommentsButtonHeight);
		}
		auto trect = inner.marginsRemoved(st::msgPadding);
		const auto reactionsTop = (reactionsInBubble && !_viewButton)
			? st::mediaInBubbleSkip
			: 0;
		const auto reactionsHeight = reactionsInBubble
			? (reactionsTop + _reactions->height())
			: 0;
		if (reactionsInBubble) {
			trect.setHeight(trect.height() - reactionsHeight);
			const auto reactionsPosition = QPoint(trect.left(), trect.top() + trect.height() + reactionsTop);
			_reactions->animate(args.translated(-reactionsPosition));
			return;
		}
	}
}

auto Message::takeEffectAnimation()
-> std::unique_ptr<Ui::ReactionFlyAnimation> {
	return _bottomInfo.takeEffectAnimation();
}

QRect Message::effectIconGeometry() const {
	const auto item = data();
	const auto media = this->media();

	auto g = countGeometry();
	if (g.width() < 1 || isHidden()) {
		return {};
	}
	const auto bubble = drawBubble();
	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	const auto mediaDisplayed = media && media->isDisplayed();
	const auto keyboard = item->inlineReplyKeyboard();
	auto keyboardHeight = 0;
	if (keyboard) {
		keyboardHeight = keyboard->naturalHeight();
		g.setHeight(g.height() - st::msgBotKbButton.margin - keyboardHeight);
	}

	const auto fromBottomInfo = [&](QPoint bottomRight) {
		const auto size = _bottomInfo.currentSize();
		return _bottomInfo.effectIconGeometry().translated(
			bottomRight - QPoint(size.width(), size.height()));
	};
	if (bubble) {
		const auto entry = logEntryOriginal();
		const auto check = factcheckBlock();

		// Entry page is always a bubble bottom.
		auto mediaOnBottom = (mediaDisplayed && media->isBubbleBottom()) || check || (entry/* && entry->isBubbleBottom()*/);
		auto mediaOnTop = (mediaDisplayed && media->isBubbleTop()) || (entry && entry->isBubbleTop());

		auto inner = g;
		if (_comments) {
			inner.setHeight(inner.height() - st::historyCommentsButtonHeight);
		}
		auto trect = inner.marginsRemoved(st::msgPadding);
		const auto reactionsTop = (reactionsInBubble && !_viewButton)
			? st::mediaInBubbleSkip
			: 0;
		const auto reactionsHeight = reactionsInBubble
			? (reactionsTop + _reactions->height())
			: 0;
		if (_viewButton) {
			const auto belowInfo = _viewButton->belowMessageInfo();
			const auto infoHeight = reactionsInBubble
				? (reactionsHeight + 2 * st::mediaInBubbleSkip)
				: _bottomInfo.height();
			const auto heightMargins = QMargins(0, 0, 0, infoHeight);
			if (belowInfo) {
				inner -= heightMargins;
			}
			trect.setHeight(trect.height() - _viewButton->height());
			if (reactionsInBubble) {
				trect.setHeight(trect.height() - st::mediaInBubbleSkip + st::msgPadding.bottom());
			} else if (mediaDisplayed) {
				trect.setHeight(trect.height() - st::mediaInBubbleSkip);
			}
		}
		if (mediaOnBottom) {
			trect.setHeight(trect.height()
				+ st::msgPadding.bottom()
				- viewButtonHeight());
		}
		if (mediaOnTop) {
			trect.setY(trect.y() - st::msgPadding.top());
		}
		if (mediaDisplayed && mediaOnBottom && media->customInfoLayout()) {
			auto mediaHeight = media->height();
			auto mediaLeft = trect.x() - st::msgPadding.left();
			auto mediaTop = (trect.y() + trect.height() - mediaHeight);
			return fromBottomInfo(QPoint(mediaLeft, mediaTop) + media->resolveCustomInfoRightBottom());
		} else {
			return fromBottomInfo({
				inner.left() + inner.width() - (st::msgPadding.right() - st::msgDateDelta.x()),
				inner.top() + inner.height() - (st::msgPadding.bottom() - st::msgDateDelta.y()),
			});
		}
	} else if (mediaDisplayed) {
		return fromBottomInfo(g.topLeft() + media->resolveCustomInfoRightBottom());
	}
	return {};
}

QSize Message::performCountOptimalSize() {
	invalidateTopicButtonNameRepaint();
	invalidateTopicButtonRippleRepaint();
	const auto item = data();

	const auto replyData = item->Get<HistoryMessageReply>();
	const auto &summary = item->summaryEntry();
	const auto showSummaryReply = !summary.result.empty() && summary.shown;

	if (replyData && !_hideReply) {
		AddComponents(Reply::Bit());
	} else {
		RemoveComponents(Reply::Bit());
	}
	if (showSummaryReply) {
		AddComponents(SummaryHeader::Bit());
	} else {
		RemoveComponents(SummaryHeader::Bit());
	}

	if (item->history()->peer->isMonoforum()) {
		if (const auto suggest = item->Get<HistoryMessageSuggestion>()) {
			if (const auto service = Get<ServicePreMessage>()) {
				// Ok, we didn't have the message, but now we have.
				// That means this is not a plain post suggestion,
				// but a suggestion of changes to previous suggestion.
				if (service->media
					&& !service->handler
					&& replyData
					&& replyData->resolvedMessage) {
					refreshSuggestedInfo(item, suggest, replyData);
				}
			}
		}
	}

	if (const auto postSender = item->discussionPostOriginalSender()) {
		if (!postSender->isFullLoaded()) {
			// We need it for available reactions list.
			postSender->updateFull();
		}
	}

	const auto factcheck = item->Get<HistoryMessageFactcheck>();
	if (factcheck && !factcheck->data.text.empty()) {
		AddComponents(Factcheck::Bit());
		Get<Factcheck>()->page = history()->session().factchecks().makeMedia(
			this,
			factcheck);
	} else {
		RemoveComponents(Factcheck::Bit());
	}
	refreshRightBadge();

	const auto markup = item->inlineReplyMarkup();
	const auto reactionsKey = [&] {
		return embedReactionsInBubble() ? 0 : 1;
	};
	const auto oldKey = reactionsKey();
	if (_summarize) {
		const auto &summary = item->summaryEntry();
		if (_summarize->loading() != summary.loading) {
			_summarize->setLoading(summary.loading);
		}
	}
	validateText();
	validateInlineKeyboard(markup);
	updateViewButtonExistence();
	refreshTopicButton();

	const auto media = this->media();
	const auto textItem = this->textItem();
	const auto defaultInvert = media && media->aboveTextByDefault();
	const auto invertDefault = textItem
		&& textItem->invertMedia()
		&& !textItem->emptyText();
	_invertMedia = invertDefault ? !defaultInvert : defaultInvert;

	updateMediaInBubbleState();
	if (oldKey != reactionsKey()) {
		refreshReactions();
	}
	refreshInfoSkipBlock(textItem);

	const auto botTop = item->isFakeAboutView()
		? Get<FakeBotAboutTop>()
		: nullptr;
	const auto ephemeralBadge = Get<EphemeralBadge>();
	const auto bubble = drawBubble();
	auto withVisibleText = false;
	auto fullTextualWidth = 0;
	if (botTop) {
		botTop->init();
	}
	if (ephemeralBadge) {
		ephemeralBadge->init(item);
	}

	auto maxWidth = 0;
	auto minHeight = 0;

	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	if (_reactions) {
		_reactions->initDimensions();
	}

	const auto reply = Get<Reply>();
	if (reply) {
		reply->update(this, replyData);
	}
	const auto summaryHeader = Get<SummaryHeader>();
	if (summaryHeader) {
		if (showSummaryReply) {
			summaryHeader->update(this);
		}
	}

	if (bubble) {
		const auto forwarded = item->Get<HistoryMessageForwarded>();
		const auto via = item->Get<HistoryMessageVia>();
		const auto entry = logEntryOriginal();
		const auto check = factcheckBlock();
		if (forwarded) {
			forwarded->create(via, item);
		}

		auto mediaDisplayed = false;
		if (media) {
			mediaDisplayed = media->isDisplayed();
			media->initDimensions();
		}
		if (check) {
			check->initDimensions();
		}
		if (entry) {
			entry->initDimensions();
		}

		// Entry page is always a bubble bottom.
		withVisibleText = hasVisibleText();
		fullTextualWidth = textualMaxWidth();
		const auto textualWidth = bubbleTextualWidth();
		auto mediaOnBottom = (mediaDisplayed && media->isBubbleBottom()) || check || (entry/* && entry->isBubbleBottom()*/);
		auto mediaOnTop = (mediaDisplayed && media->isBubbleTop()) || (entry && entry->isBubbleTop());
		maxWidth = textualWidth;
		auto nonTextMax = 0;
		if (isCommentsRootView()) {
			maxWidth = std::max(maxWidth, st::msgMaxWidth);
			accumulate_max(nonTextMax, st::msgMaxWidth);
		}
		minHeight = withVisibleText
			? hasRichPage()
				? textHeightFor(bubbleTextWidth(textualWidth))
				: text().minHeight()
			: 0;
		if (reactionsInBubble) {
			const auto reactionsMaxWidth = st::msgPadding.left()
				+ _reactions->maxWidth()
				+ st::msgPadding.right();
			const auto reactionsLimited = std::min(
				st::msgMaxWidth,
				reactionsMaxWidth);
			accumulate_max(maxWidth, reactionsLimited);
			accumulate_max(nonTextMax, reactionsLimited);
			if (mediaDisplayed
				&& !media->additionalInfoString().isEmpty()) {
				// In round videos in a web page status text is painted
				// in the bottom left corner, reactions should be below.
				minHeight += st::msgDateFont->height;
			} else {
				minHeight += st::mediaInBubbleSkip;
			}
			if (maxWidth >= reactionsMaxWidth) {
				minHeight += _reactions->minHeight();
			} else {
				const auto widthForReactions = maxWidth
					- st::msgPadding.left()
					- st::msgPadding.right();
				minHeight += _reactions->resizeGetHeight(widthForReactions);
			}
		}
		if (!mediaOnBottom && (!_viewButton || !reactionsInBubble)) {
			minHeight += st::msgPadding.bottom();
			if (mediaDisplayed) {
				minHeight += st::mediaInBubbleSkip;
			}
		}
		if (!mediaOnTop) {
			minHeight += st::msgPadding.top();
			if (mediaDisplayed) minHeight += st::mediaInBubbleSkip;
			if (entry) minHeight += st::mediaInBubbleSkip;
		}
		if (check) minHeight += st::mediaInBubbleSkip;
		if (mediaDisplayed) {
			// Parts don't participate in maxWidth() in case of media message.
			if (media->enforceBubbleWidth()) {
				maxWidth = media->maxWidth();
				const auto innerWidth = maxWidth
					- st::msgPadding.left()
					- st::msgPadding.right();
				if (reactionsInBubble) {
					minHeight -= _reactions->minHeight();
					minHeight
						+= _reactions->countCurrentSize(innerWidth).height();
				}
			} else {
				accumulate_max(maxWidth, media->maxWidth());
			}
			minHeight += media->minHeight();
		} else {
			// Count parts in maxWidth(), don't count them in minHeight().
			// They will be added in resizeGetHeight() anyway.
			if (displayFromName()) {
				const auto from = item->displayFrom();
				validateFromNameText(from);
				const auto &name = from
					? _fromName
					: item->displayHiddenSenderInfo()->nameText();
				auto namew = st::msgPadding.left()
					+ name.maxWidth()
					+ (_fromNameStatus
						? st::dialogsPremiumIcon.icon.width()
						: 0)
					+ st::msgPadding.right();
				if (via && !displayForwardedFrom()) {
					namew += st::msgServiceFont->spacew + via->maxWidth
						+ (_fromNameStatus ? st::msgServiceFont->spacew : 0);
				}
				if (const auto guestChat = item->Get<HistoryMessageGuestChat>()) {
					namew += st::msgServiceFont->spacew + guestChat->maxWidth;
				}
				if (Has<RightBadge>()) {
					namew += st::msgPadding.right() + rightBadgeWidth();
				}
				accumulate_max(maxWidth, namew);
				accumulate_max(nonTextMax, namew);
			} else if (via && !displayForwardedFrom()) {
				const auto viaw = st::msgPadding.left() + via->maxWidth + st::msgPadding.right();
				accumulate_max(maxWidth, viaw);
				accumulate_max(nonTextMax, viaw);
			}
			if (displayedTopicButton()) {
				const auto padding = st::msgPadding + st::topicButtonPadding;
				const auto topicw = padding.left()
					+ _topicButton->name.maxWidth()
					+ st::topicButtonArrowSkip
					+ padding.right();
				accumulate_max(maxWidth, topicw);
				accumulate_max(nonTextMax, topicw);
			}
			if (displayForwardedFrom()) {
				const auto skip1 = forwarded->psaType.isEmpty()
					? 0
					: st::historyPsaIconSkip1;
				auto namew = st::msgPadding.left() + forwarded->text.maxWidth() + skip1 + st::msgPadding.right();
				if (via) {
					namew += st::msgServiceFont->spacew + via->maxWidth;
				}
				accumulate_max(maxWidth, namew);
				accumulate_max(nonTextMax, namew);
			}
			if (reply) {
				const auto replyw = st::msgPadding.left()
					+ reply->maxWidth()
					+ st::msgPadding.right();
				accumulate_max(maxWidth, replyw);
				accumulate_max(nonTextMax, replyw);
			}
			if (summaryHeader) {
				const auto summaryHeaderWidth = st::msgPadding.left()
					+ summaryHeader->maxWidth()
					+ st::msgPadding.right();
				accumulate_max(maxWidth, summaryHeaderWidth);
				accumulate_max(nonTextMax, summaryHeaderWidth);
			}
			if (check) {
				accumulate_max(maxWidth, check->maxWidth());
				accumulate_max(nonTextMax, check->maxWidth());
				minHeight += check->minHeight();
			}
			if (entry) {
				accumulate_max(maxWidth, entry->maxWidth());
				accumulate_max(nonTextMax, entry->maxWidth());
				minHeight += entry->minHeight();
			}
		}
		if (withVisibleText && botTop) {
			accumulate_max(maxWidth, botTop->maxWidth);
			accumulate_max(nonTextMax, botTop->maxWidth);
			minHeight += botTop->height;
		}
		if (ephemeralBadge) {
			accumulate_max(maxWidth, ephemeralBadge->maxWidth);
			accumulate_max(nonTextMax, ephemeralBadge->maxWidth);
		}
		accumulate_max(maxWidth, minWidthForMedia());
		accumulate_max(nonTextMax, minWidthForMedia());
		_nonTextMaxWidth = std::min(nonTextMax, kMaxWidth);
	} else if (media) {
		media->initDimensions();
		maxWidth = media->maxWidth();
		minHeight = media->isDisplayed() ? media->minHeight() : 0;
	} else {
		maxWidth = st::msgMinWidth;
		minHeight = 0;
	}
	// if we have a text bubble we can resize it to fit the keyboard
	// but if we have only media we don't do that
	if (markup && markup->inlineKeyboard && hasVisibleText()) {
		accumulate_max(maxWidth, markup->inlineKeyboard->naturalWidth());
		if (bubble) {
			const auto kbw = markup->inlineKeyboard->naturalWidth();
			if (kbw > int(_nonTextMaxWidth)) {
				_nonTextMaxWidth = std::min(kbw, kMaxWidth);
			}
		}
	}
	if (bubble && withVisibleText && maxWidth < fullTextualWidth) {
		minHeight -= hasRichPage()
			? textHeightFor(bubbleTextWidth(bubbleTextualWidth()))
			: text().minHeight();
		minHeight += textHeightFor(bubbleTextWidth(maxWidth));
	}
	if (const auto appearing = Get<TextAppearing>()) {
		appearing->geometryValid = false;
		appearing->startedForText = false;
		appearing->finalizing = item->isRegular();
	}
	return QSize(maxWidth, minHeight);
}

void Message::refreshTopicButton() {
	const auto item = data();
	if (isAttachedToPrevious() || delegate()->elementHideTopicButton(this)) {
		resetTopicButton();
	} else if (const auto topic = item->topic()) {
		if (topic->peer()->useSubsectionTabs()) {
			resetTopicButton();
			return;
		}
		if (!_topicButton) {
			_topicButton = std::make_unique<TopicButton>();
		}
		const auto jumpToId = IsServerMsgId(item->id) ? item->id : MsgId();
		_topicButton->link = MakeTopicButtonLink(topic, jumpToId);
		if (_topicButton->nameVersion != topic->titleVersion()) {
			invalidateTopicButtonNameRepaint();
			invalidateTopicButtonRippleRepaint();
			clearTopicButtonRipple();
			_topicButton->nameVersion = topic->titleVersion();
			const auto generation = ++_topicButtonGeneration;
			_topicButton->nameRepaint.generation = generation;
			const auto weak = base::make_weak(this);
			const auto context = Core::TextContext({
				.session = &history()->session(),
				.repaint = [weak, generation] {
					if (const auto strong = weak.get()) {
						strong->repaintTopicButtonName(generation);
					}
				},
				.customEmojiLoopLimit = 1,
			});
			_topicButton->name.setMarkedText(
				st::fwdTextStyle,
				topic->titleWithIcon(),
				kMarkupTextOptions,
				context);
		}
	} else {
		resetTopicButton();
	}
}

void Message::resetTopicButton() {
	if (!_topicButton) {
		return;
	}
	invalidateTopicButtonRippleRepaint();
	clearTopicButtonRipple();
	++_topicButtonGeneration;
	_topicButton->nameRepaint = TopicButton::NameRepaint();
	_topicButton = nullptr;
}

void Message::clearTopicButtonRipple() const {
	if (!_topicButton || !_topicButton->ripple) {
		return;
	}
	auto &repaint = ensureTopicButtonRippleRepaint();
	repaint.maskSize = QSize();
	repaint.pending = false;
	++repaint.generation;
	_topicButton->ripple = nullptr;
}

void Message::repaintTopicButtonRipple(uint64 generation) const {
	const auto repaint = _topicButtonRippleRepaint.get();
	if (!_topicButton
		|| !_topicButton->ripple
		|| !repaint
		|| repaint->generation != generation
		|| repaint->pending
		|| (repaint->known && repaint->current.isEmpty())) {
		return;
	}
	repaint->pending = true;
	if (!repaint->known) {
		Ui::LogUnknownGeometryRepaint("topic button ripple");
		this->repaint();
	} else {
		repaintTopicButtonRippleRegion(repaint->current);
	}
}

void Message::recordTopicButtonRippleRepaint(
		const Painter &p,
		const PaintContext &context,
		QRect rect) const {
	if (!context.hasElementPainter(p)) {
		return;
	} else if (!_topicButtonRippleRepaint && rect.isEmpty()) {
		return;
	}
	auto &repaint = ensureTopicButtonRippleRepaint();
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
	repaint.pending = false;
	repaint.current = known ? std::move(current) : QRegion();
	repaint.known = known;
	if (!known) {
		repaint.stale = previous;
		if (!previous.isEmpty()) {
			repaint.pending = true;
			Ui::LogUnknownGeometryRepaint("topic button ripple");
			this->repaint();
		}
		return;
	} else if (previous.isEmpty()
		|| (stale.isEmpty() && previous == repaint.current)) {
		return;
	}
	repaint.pending = true;
	repaintTopicButtonRippleRegion(previous.united(repaint.current));
}

void Message::invalidateTopicButtonRippleRepaint() const {
	if (const auto repaint = _topicButtonRippleRepaint.get()) {
		repaint->stale = repaint->stale.united(base::take(repaint->current));
		repaint->pending = false;
		repaint->known = false;
	}
}

void Message::repaintTopicButtonRippleRegion(const QRegion &region) const {
	repaint(region);
}

uint64 Message::resetTopicButtonRippleRepaint() const {
	auto &repaint = ensureTopicButtonRippleRepaint();
	repaint.pending = false;
	if (repaint.current.isEmpty()) {
		repaint.known = false;
	}
	return ++repaint.generation;
}

auto Message::ensureTopicButtonRippleRepaint() const
-> TopicButtonRippleRepaint & {
	if (!_topicButtonRippleRepaint) {
		_topicButtonRippleRepaint
			= std::make_unique<TopicButtonRippleRepaint>();
	}
	return *_topicButtonRippleRepaint;
}

void Message::repaintTopicButtonName(uint64 generation) const {
	const auto button = _topicButton.get();
	if (!button
		|| button->nameRepaint.generation != generation
		|| _topicButtonGeneration != generation
		|| button->nameRepaint.pending) {
		return;
	} else if (button->nameRepaint.known
		&& button->nameRepaint.current.isEmpty()) {
		return;
	}
	button->nameRepaint.pending = true;
	if (!button->nameRepaint.known) {
		Ui::LogUnknownGeometryRepaint("topic button name");
		repaint();
	} else {
		repaintTopicButtonNameRegion(button->nameRepaint.current);
	}
}

void Message::paintTopicButtonName(
		Painter &p,
		const PaintContext &context,
		QRect textRect) const {
	const auto button = displayedTopicButton();
	if (!button) {
		recordTopicButtonNameRepaint(p, context, {}, {});
		return;
	}
	prepareCustomEmojiPaint(
		p,
		context,
		button->name,
		CustomEmojiRepaintReset::No);
	auto customEmojiRepaintBounds
		= Ui::Text::CustomEmojiRepaintBounds();
	button->name.draw(p, {
		.position = textRect.topLeft(),
		.availableWidth = textRect.width(),
		.palette = &p.textPalette(),
		.paused = p.inactive(),
		.elisionLines = 1,
		.customEmojiRepaintBounds = &customEmojiRepaintBounds,
	});
	recordTopicButtonNameRepaint(
		p,
		context,
		textRect,
		customEmojiRepaintBounds);
}

void Message::recordTopicButtonNameRepaint(
		const Painter &p,
		const PaintContext &context,
		QRect textRect,
		const Ui::Text::CustomEmojiRepaintBounds
			&customEmojiRepaintBounds) const {
	const auto button = _topicButton.get();
	if (!button) {
		return;
	} else if (!context.hasElementPainter(p)) {
		return;
	}
	button->nameRepaint.pending = false;
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
		|| button->name.hasSpoilers();
	if (geometryKnown && needsTextFallback) {
		if (textRect.isEmpty()) {
			geometryKnown = customEmojiRepaintBounds.repaintBoundsKnown;
		} else if (const auto lines = button->name.countLinesGeometry(
				textRect.width()); lines.empty()) {
			geometryKnown = false;
		} else {
			const auto &line = lines.front();
			const auto elided = (lines.size() > 1)
				|| (button->name.maxWidth() > textRect.width());
			auto lineLeft = std::clamp(line.left, 0, textRect.width());
			auto lineWidth = std::clamp(line.width, 0, textRect.width());
			if (elided) {
				lineLeft = 0;
				lineWidth = textRect.width();
			} else if (line.rtl) {
				lineLeft = textRect.width() - lineWidth;
			} else {
				lineWidth = std::min(
					lineWidth,
					textRect.width() - lineLeft);
			}
			const auto lineBottom = std::clamp(
				line.bottom,
				0,
				textRect.height());
			if (lineWidth <= 0 || lineBottom <= 0) {
				geometryKnown = false;
			} else {
				const auto lineRect = QRectF(
					textRect.x() + lineLeft,
					textRect.y(),
					lineWidth,
					lineBottom);
				const auto mapped = context.mapToElement(p, lineRect);
				if (!mapped) {
					geometryKnown = false;
				} else if (!mapped->isEmpty()) {
					region += *mapped;
				}
			}
		}
	}
	if (!geometryKnown) {
		region = QRegion();
	}
	const auto stale = base::take(button->nameRepaint.stale);
	const auto previous = stale.united(
		base::take(button->nameRepaint.current));
	button->nameRepaint.current = std::move(region);
	button->nameRepaint.known = geometryKnown;
	if (previous.isEmpty()
		|| (stale.isEmpty()
			&& previous == button->nameRepaint.current)) {
		return;
	}
	button->nameRepaint.pending = true;
	repaintTopicButtonNameRegion(
		previous.united(button->nameRepaint.current));
}

void Message::invalidateTopicButtonNameRepaint() const {
	const auto button = _topicButton.get();
	if (!button) {
		return;
	}
	button->nameRepaint.stale = button->nameRepaint.stale.united(
		base::take(button->nameRepaint.current));
	button->nameRepaint.pending = false;
	button->nameRepaint.known = false;
}

void Message::repaintTopicButtonNameRegion(const QRegion &region) const {
	repaint(region);
}

void Message::repaintSelectionCheckbox() const {
	if (const auto state = _selectionCheckbox.get(); state && !state->moving) {
		RequestPaintedRectRepaint(
			this,
			state->repaint,
			"selection checkbox");
	}
}

void Message::finishSelectionCheckboxPaint(
		bool canonical,
		std::optional<QRect> paintedRect,
		bool skipConvergence) const {
	if (const auto state = _selectionCheckbox.get()) {
		if (canonical) {
			state->moving = skipConvergence;
		}
		RecordPaintedRectRepaint(
			this,
			state->repaint,
			canonical,
			paintedRect,
			skipConvergence,
			"selection checkbox");
	}
}

int Message::marginTop() const {
	auto result = 0;
	if (!isHidden()) {
		if (isAttachedToPrevious()) {
			result += st::msgMarginTopAttached;
		} else {
			result += st::msgMargin.top();
		}
	}
	result += displayedDateHeight();
	if (const auto bar = Get<UnreadBar>()) {
		result += bar->height();
	}
	if (const auto bar = Get<ForumThreadBar>()) {
		result += bar->height();
	}
	if (const auto service = Get<ServicePreMessage>()) {
		if (!service->below) {
			result += service->height;
		}
	}
	if (const auto margins = Get<ViewAddedMargins>()) {
		result += margins->top;
	}
	return result;
}

int Message::marginBottom() const {
	if (isHidden()) {
		return 0;
	}
	auto result = st::msgMargin.bottom();
	if (const auto service = Get<ServicePreMessage>()) {
		if (service->below) {
			result += service->height;
		}
	}
	if (const auto margins = Get<ViewAddedMargins>()) {
		result += margins->bottom;
	}
	return result;
}

void Message::draw(Painter &p, const PaintContext &context) const {
	const auto canonicalPaint = context.hasElementPainter(p);
	auto fromNameStatusRepaintRect = std::optional<QRect>(QRect());
	const auto fromNameStatusPaintGuard = gsl::finally([&] {
		if (canonicalPaint) {
			finishFromNameStatusPaint(fromNameStatusRepaintRect);
		}
	});
	const auto selectionCheckboxCanonical
		= canonicalPaint && !context.skipSelectionCheck;
	auto selectionCheckboxPaintedRect = selectionCheckboxCanonical
		? std::optional<QRect>(QRect())
		: std::optional<QRect>();
	auto selectionCheckboxMoving = false;
	auto clearSelectionCheckbox = false;
	const auto selectionCheckboxPaintGuard = gsl::finally([&] {
		finishSelectionCheckboxPaint(
			selectionCheckboxCanonical,
			selectionCheckboxPaintedRect,
			selectionCheckboxMoving);
		if (clearSelectionCheckbox && selectionCheckboxCanonical) {
			_selectionCheckbox = nullptr;
		}
	});
	auto g = countGeometry();
	if (g.width() < 1) {
		recordTextRepaintRect(p, context, {});
		recordTopicButtonNameRepaint(p, context, {}, {});
		recordTopicButtonRippleRepaint(p, context, QRect());
		return;
	}
	const auto item = data();
	const auto media = this->media();

	const auto hasGesture = context.gestureHorizontal.translation
		&& (context.gestureHorizontal.msgBareId == item->fullId().msg.bare);
	if (hasGesture) {
		p.translate(context.gestureHorizontal.translation, 0);
	}
	const auto selectionModeResult = delegate()->elementInSelectionMode(this);
	selectionCheckboxMoving = selectionModeResult.progress > 0.
		&& selectionModeResult.progress != 1.;
	const auto selectionTranslation = (selectionModeResult.progress > 0)
		? (selectionModeResult.progress
			* AdditionalSpaceForSelectionCheckbox(this, g))
		: 0;
	if (selectionTranslation) {
		p.translate(selectionTranslation, 0);
	}

	if (item->hasUnrequestedFactcheck()) {
		item->history()->session().factchecks().requestFor(item);
	}

	const auto stm = context.messageStyle();
	const auto bubble = drawBubble();

	if (const auto bar = Get<UnreadBar>()) {
		auto unreadbarh = bar->height();
		auto aboveh = 0;
		if (const auto date = Get<DateBadge>()) {
			aboveh += date->height();
		}
		if (const auto bar = Get<ForumThreadBar>()) {
			aboveh += bar->height();
		}
		if (context.clip.intersects(QRect(0, aboveh, width(), unreadbarh))) {
			p.translate(0, aboveh);
			bar->paint(p, context, 0, width(), delegate()->elementChatMode());
			p.translate(0, -aboveh);
		}
	}

	if (const auto service = Get<ServicePreMessage>()) {
		service->paint(p, context, g, delegate()->elementChatMode());
	}

	if (isHidden()) {
		recordTextRepaintRect(p, context, {});
		recordTopicButtonNameRepaint(p, context, {}, {});
		recordTopicButtonRippleRepaint(p, context, QRect());
		return;
	}

	const auto entry = logEntryOriginal();
	const auto check = factcheckBlock();
	auto mediaDisplayed = media && media->isDisplayed();

	// Entry page is always a bubble bottom.
	auto mediaOnBottom = (mediaDisplayed && media->isBubbleBottom()) || check || (entry/* && entry->isBubbleBottom()*/);
	auto mediaOnTop = (mediaDisplayed && media->isBubbleTop()) || (entry && entry->isBubbleTop());

	const auto displayInfo = needInfoDisplay();
	const auto reactionsInBubble = _reactions && embedReactionsInBubble();

	// We need to count geometry without keyboard and reactions
	// for bubble selection intervals counting below.
	auto gForIntervals = g;
	if (_reactions && !reactionsInBubble) {
		const auto reactionsHeight = st::mediaInBubbleSkip + _reactions->height();
		gForIntervals.setHeight(gForIntervals.height() - reactionsHeight);
	}
	const auto keyboard = item->inlineReplyKeyboard();
	if (keyboard) {
		const auto keyboardHeight = st::msgBotKbButton.margin + keyboard->naturalHeight();
		gForIntervals.setHeight(gForIntervals.height() - keyboardHeight);
	}

	auto mediaSelectionIntervals = (!context.selected() && mediaDisplayed)
		? media->getBubbleSelectionIntervals(context.selection)
		: std::vector<Ui::BubbleSelectionInterval>();
	auto localMediaTop = 0;
	const auto customHighlight = mediaDisplayed && media->customHighlight();
	if (!mediaSelectionIntervals.empty() || customHighlight) {
		auto localMediaBottom = gForIntervals.top() + gForIntervals.height();
		if (data()->repliesAreComments() || data()->externalReply()) {
			localMediaBottom -= st::historyCommentsButtonHeight;
		}
		if (_viewButton) {
			localMediaBottom -= st::mediaInBubbleSkip + _viewButton->height();
		}
		if (reactionsInBubble) {
			localMediaBottom -= st::mediaInBubbleSkip + _reactions->height();
		}
		if (!mediaOnBottom && (!_viewButton || !reactionsInBubble)) {
			localMediaBottom -= st::msgPadding.bottom();
			if (mediaDisplayed) {
				localMediaBottom -= st::mediaInBubbleSkip;
			}
		}
		if (check) {
			localMediaBottom -= check->height();
		}
		if (entry) {
			localMediaBottom -= entry->height();
		}
		localMediaTop = localMediaBottom - media->height();
		for (auto &[top, height] : mediaSelectionIntervals) {
			top += localMediaTop;
		}
	}

	{
		if (selectionTranslation) {
			p.translate(-selectionTranslation, 0);
		}
		if (customHighlight) {
			media->drawHighlight(p, context, localMediaTop);
		} else {
			paintHighlight(p, context, g.height());
		}
		if (selectionTranslation) {
			p.translate(selectionTranslation, 0);
		}
	}

	const auto roll = media ? media->bubbleRoll() : Media::BubbleRoll();
	if (roll) {
		p.save();
		p.translate(g.center());
		p.rotate(roll.rotate);
		p.scale(roll.scale, roll.scale);
		p.translate(-g.center());
	}

	p.setTextPalette(stm->textPalette);

	if (_reactions && !reactionsInBubble) {
		const auto reactionsHeight = st::mediaInBubbleSkip + _reactions->height();
		const auto reactionsLeft = (!bubble && mediaDisplayed)
			? media->contentRectForReactions().x()
			: 0;
		g.setHeight(g.height() - reactionsHeight);
		const auto reactionsPosition = QPoint(reactionsLeft + g.left(), g.top() + g.height() + st::mediaInBubbleSkip);
		p.translate(reactionsPosition);
		prepareCustomEmojiPaint(p, context, *_reactions);
		_reactions->paint(p, context, g.width(), context.clip.translated(-reactionsPosition));
		if (context.reactionInfo) {
			context.reactionInfo->position = reactionsPosition;
		}
		p.translate(-reactionsPosition);
	}

	const auto messageRounding = countMessageRounding();
	if (keyboard) {
		const auto keyboardHeight = st::msgBotKbButton.margin + keyboard->naturalHeight();
		g.setHeight(g.height() - keyboardHeight);

		const auto keyboardPosition = QPoint(g.left(), g.top() + g.height() + st::msgBotKbButton.margin);
		p.translate(keyboardPosition);
		keyboard->paint(
			p,
			context.st,
			messageRounding,
			g.width(),
			context.clip.translated(-keyboardPosition),
			context.paused,
			[&](const ReplyKeyboardButtonPaint &painted) {
				recordInlineKeyboardAnimationPaint(
					p,
					context,
					keyboard,
					painted);
			});
		p.translate(-keyboardPosition);
	}

	if (context.highlightPathCache) {
		context.highlightInterpolateTo = g;
		context.highlightPathCache->clear();
	}
	if (bubble) {
		if (displayFromName()
			&& item->displayFrom()
			&& (_fromNameVersion < item->displayFrom()->nameVersion())) {
			fromNameUpdated(g.width());
		}
		Ui::PaintBubble(
			p,
			Ui::ComplexBubble{
				.simple = Ui::SimpleBubble{
					.st = context.st,
					.geometry = g,
					.pattern = context.bubblesPattern,
					.patternViewport = context.viewport,
					.outerWidth = width(),
					.selected = context.selected(),
					.outbg = context.outbg,
					.rounding = countBubbleRounding(messageRounding),
				},
				.selection = mediaSelectionIntervals,
			});

		auto inner = g;
		paintCommentsButton(p, inner, context);

		auto trect = inner.marginsRemoved(st::msgPadding);

		const auto additionalInfoSkip = (mediaDisplayed
			&& !media->additionalInfoString().isEmpty())
			? st::msgDateFont->height
			: 0;
		const auto reactionsTop = (reactionsInBubble && !_viewButton)
			? (additionalInfoSkip + st::mediaInBubbleSkip)
			: additionalInfoSkip;
		const auto reactionsHeight = reactionsInBubble
			? (reactionsTop + _reactions->height())
			: 0;
		if (reactionsInBubble) {
			trect.setHeight(trect.height() - reactionsHeight);
			const auto reactionsPosition = QPoint(trect.left(), trect.top() + trect.height() + reactionsTop);
			p.translate(reactionsPosition);
			prepareCustomEmojiPaint(p, context, *_reactions);
			_reactions->paint(p, context, g.width(), context.clip.translated(-reactionsPosition));
			if (context.reactionInfo) {
				context.reactionInfo->position = reactionsPosition;
			}
			p.translate(-reactionsPosition);
		}

		if (_viewButton) {
			const auto belowInfo = _viewButton->belowMessageInfo();
			const auto infoHeight = reactionsInBubble
				? (reactionsHeight + 2 * st::mediaInBubbleSkip)
				: _bottomInfo.height();
			const auto heightMargins = QMargins(0, 0, 0, infoHeight);
			_viewButton->draw(
				p,
				_viewButton->countRect(belowInfo
					? inner
					: inner - heightMargins),
				context);
			if (belowInfo) {
				inner.setHeight(inner.height() - _viewButton->height());
			}
			trect.setHeight(trect.height() - _viewButton->height());
			if (reactionsInBubble) {
				trect.setHeight(trect.height() - st::mediaInBubbleSkip + st::msgPadding.bottom());
			} else if (mediaDisplayed) {
				trect.setHeight(trect.height() - st::mediaInBubbleSkip);
			}
		}

		if (mediaOnBottom) {
			trect.setHeight(trect.height() + st::msgPadding.bottom());
		}
		if (mediaOnTop) {
			recordTopicButtonNameRepaint(p, context, {}, {});
			recordTopicButtonRippleRepaint(p, context, QRect());
			trect.setY(trect.y() - st::msgPadding.top());
		} else {
			paintFromName(
				p,
				trect,
				context,
				canonicalPaint
					? &fromNameStatusRepaintRect
					: nullptr);
			paintEphemeralBadge(p, trect, context);
			paintTopicButton(p, trect, context);
			paintForwardedInfo(p, trect, context);
			paintViaBotIdInfo(p, trect, context);
			paintReplyInfo(p, trect, context);
			paintSummaryHeaderInfo(p, trect, context);
		}
		if (entry) {
			trect.setHeight(trect.height() - entry->height());
		}
		if (check) {
			trect.setHeight(trect.height() - check->height() - st::mediaInBubbleSkip);
		}
		if (displayInfo) {
			trect.setHeight(trect.height()
				- (_bottomInfo.height() - st::msgDateFont->height));
		}
		auto textSelection = context.selection;
		auto highlightRange = context.highlight.range;
		const auto mediaHeight = mediaDisplayed ? media->height() : 0;
		const auto paintMedia = [&](int top) {
			if (!mediaDisplayed) {
				return;
			}
			const auto mediaSelection = _invertMedia
				? context.selection
				: skipTextSelection(context.selection);
			const auto maybeMediaHighlight = context.highlightPathCache
				&& context.highlightPathCache->isEmpty();
			auto mediaPosition = QPoint(inner.left(), top);
			_lastMediaPosition = mediaPosition;
			p.translate(mediaPosition);
			media->draw(p, context.translated(
				-mediaPosition
			).withSelection(mediaSelection));
			if (context.reactionInfo && !displayInfo && !_reactions) {
				const auto add = QPoint(0, mediaHeight);
				context.reactionInfo->position = mediaPosition + add;
				if (context.reactionInfo->effectPaint) {
					context.reactionInfo->effectOffset -= add;
				}
			}
			if (maybeMediaHighlight
				&& !context.highlightPathCache->isEmpty()) {
				context.highlightPathCache->translate(mediaPosition);
			}
			p.translate(-mediaPosition);
		};
		if (mediaDisplayed && _invertMedia) {
			if (!mediaOnTop) {
				trect.setY(trect.y() + st::mediaInBubbleSkip);
			}
			paintMedia(trect.y());
			trect.setY(trect.y()
				+ mediaHeight
				+ (mediaOnBottom ? 0 : st::mediaInBubbleSkip));
			textSelection = media->skipSelection(textSelection);
			highlightRange = media->skipSelection(highlightRange);
		}
		const auto drawText = context.skipDrawingParts
			!= PaintContext::SkipDrawingParts::Content;
		const auto drawOnlyText = drawText
			&& (context.skipDrawingParts
				!= PaintContext::SkipDrawingParts::None);
		if (drawOnlyText) {
			p.save();
			p.setClipping(false);
		}
		if (drawText) {
			auto copy = context;
			copy.selection = textSelection;
			copy.highlight.range = highlightRange;
			paintText(p, trect, copy);
		}
		if (drawOnlyText) {
			p.restore();
		}
		if (mediaDisplayed && !_invertMedia) {
			paintMedia(trect.y() + trect.height() - mediaHeight);
			if (context.reactionInfo && !displayInfo && !_reactions) {
				context.reactionInfo->position
					= QPoint(inner.left(), trect.y() + trect.height());
				if (context.reactionInfo->effectPaint) {
					context.reactionInfo->effectOffset -= QPoint(0, mediaHeight);
				}
			}
		}
		if (check) {
			auto checkLeft = inner.left();
			auto checkTop = trect.y() + trect.height() + st::mediaInBubbleSkip;
			p.translate(checkLeft, checkTop);
			auto checkContext = context.translated(checkLeft, -checkTop);
			checkContext.selection = skipTextSelection(context.selection);
			if (mediaDisplayed) {
				checkContext.selection = media->skipSelection(
					checkContext.selection);
			}
			check->draw(p, checkContext);
			p.translate(-checkLeft, -checkTop);
		}
		if (entry) {
			auto entryLeft = inner.left();
			auto entryTop = trect.y() + trect.height();
			p.translate(entryLeft, entryTop);
			auto entryContext = context.translated(-entryLeft, -entryTop);
			entryContext.selection = skipTextSelection(context.selection);
			if (mediaDisplayed) {
				entryContext.selection = media->skipSelection(
					entryContext.selection);
			}
			entry->draw(p, entryContext);
			p.translate(-entryLeft, -entryTop);
		}
		if (displayInfo) {
			const auto bottomSelected = context.selected()
				|| (!mediaSelectionIntervals.empty()
					&& (mediaSelectionIntervals.back().top
						+ mediaSelectionIntervals.back().height
						>= inner.y() + inner.height()));
			drawInfo(
				p,
				context.withSelection(
					bottomSelected ? FullSelection : TextSelection()),
				inner.left() + inner.width(),
				inner.top() + inner.height(),
				2 * inner.left() + inner.width(),
				InfoDisplayType::Default);
			if (context.reactionInfo && !_reactions) {
				const auto add = QPoint(0, inner.top() + inner.height());
				context.reactionInfo->position = add;
				if (context.reactionInfo->effectPaint) {
					context.reactionInfo->effectOffset -= add;
				}
			}
			if (_comments) {
				const auto o = p.opacity();
				p.setOpacity(0.3);
				p.fillRect(g.left(), g.top() + g.height() - st::historyCommentsButtonHeight - st::lineWidth, g.width(), st::lineWidth, stm->msgDateFg);
				p.setOpacity(o);
			}
		}
		ensureSummarizeButton();
		if (const auto size = rightActionSize(); size || _summarize) {
			const auto rightActionWidth = size
				? size->width()
				: _summarize->size().width();
			const auto fastShareSkip = size
				? std::clamp(
					(g.height() - size->height()) / 2,
					0,
					st::historyFastShareBottom)
				: st::historyFastShareBottom;
			const auto fastShareLeft = hasRightLayout()
				? (g.left()
					- (_summarize ? 0 : rightActionWidth)
					- st::historyFastShareLeft)
				: (g.left() + g.width() + st::historyFastShareLeft);
			const auto fastShareTop = g.top() + (data()->isSponsored()
				? fastShareSkip
				: g.height() - fastShareSkip - (size ? size->height() : 0));
			if (size) {
				const auto o = p.opacity();
				if (selectionModeResult.progress > 0) {
					p.setOpacity(1. - selectionModeResult.progress);
				}
				drawRightAction(
					p,
					context,
					fastShareLeft,
					fastShareTop,
					width());
				if (selectionModeResult.progress > 0) {
					p.setOpacity(o);
				}
			}
			if (_summarize) {
				paintSummarize(
					p,
					fastShareLeft,
					fastShareTop,
					!context.outbg,
					context,
					g);
			}
		}

		if (media) {
			media->paintBubbleFireworks(p, g, context.now);
		}
	} else if (media && media->isDisplayed()) {
		recordTextRepaintRect(p, context, {});
		recordTopicButtonRippleRepaint(p, context, QRect());
		p.translate(g.topLeft());
		media->draw(p, context.translated(
			-g.topLeft()
		).withSelection(skipTextSelection(context.selection)));
		if (context.reactionInfo && !_reactions) {
			const auto add = QPoint(0, g.height());
			context.reactionInfo->position = g.topLeft() + add;
			if (context.reactionInfo->effectPaint) {
				context.reactionInfo->effectOffset -= add;
			}
		}
		p.translate(-g.topLeft());
	} else {
		recordTextRepaintRect(p, context, {});
		recordTopicButtonRippleRepaint(p, context, QRect());
	}

	p.restoreTextPalette();

	if (context.highlightPathCache
		&& !context.highlightPathCache->isEmpty()) {
		const auto alpha = int(0.25
			* context.highlight.collapsion
			* context.highlight.opacity
			* 255);
		if (alpha > 0) {
			context.highlightPathCache->setFillRule(Qt::WindingFill);
			auto color = context.messageStyle()->textPalette.linkFg->c;
			color.setAlpha(alpha);
			p.fillPath(*context.highlightPathCache, color);
		}
	}

	if (roll) {
		p.restore();
	}

	if (const auto reply = Get<Reply>()) {
		if (const auto replyData = item->Get<HistoryMessageReply>()) {
			if (reply->isNameUpdated(this, replyData)) {
				const_cast<Message*>(this)->setPendingResize();
			}
		}
	}
	if (hasGesture) {
		p.translate(-context.gestureHorizontal.translation, 0);

		constexpr auto kShiftRatio = 1.5;
		constexpr auto kBouncePart = 0.25;
		constexpr auto kMaxHeightRatio = 3.5;
		constexpr auto kStrokeWidth = 2.;
		constexpr auto kWaveWidth = 10.;
		const auto isLeftSize = !context.outbg
			|| (delegate()->elementChatMode() == ElementChatMode::Wide);
		const auto ratio = std::min(context.gestureHorizontal.ratio, 1.);
		const auto reachRatio = context.gestureHorizontal.reachRatio;
		const auto size = st::historyFastShareSize;
		const auto outerWidth = st::historySwipeIconSkip
			+ (isLeftSize ? rect::right(g) : width())
			+ ((g.height() < size * kMaxHeightRatio)
				? rightActionSize().value_or(QSize()).width()
				: 0);
		const auto shift = std::min(
			(size * kShiftRatio * context.gestureHorizontal.ratio),
			-1. * context.gestureHorizontal.translation
		) + (st::historySwipeIconSkip * ratio * (isLeftSize ? .7 : 1.));
		const auto rect = QRectF(
			outerWidth - shift,
			g.y() + (g.height() - size) / 2,
			size,
			size);
		const auto center = rect::center(rect);
		const auto spanAngle = ratio * arc::kFullLength;
		const auto strokeWidth = style::ConvertFloatScale(kStrokeWidth);

		const auto reachScale = std::clamp(
			(reachRatio > kBouncePart)
				? (kBouncePart * 2 - reachRatio)
				: reachRatio,
			0.,
			1.);
		auto pen = Window::Theme::IsNightMode()
			? QPen(anim::with_alpha(context.st->msgServiceFg()->c, 0.3))
			: QPen(context.st->msgServiceBg());
		pen.setWidthF(strokeWidth - (1. * (reachScale / kBouncePart)));
		const auto arcRect = rect - Margins(strokeWidth);
		p.save();
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(context.st->msgServiceBg());
			p.setOpacity(ratio);
			p.translate(center);
			if (reachScale) {
				p.scale(-(1. + 1. * reachScale), (1. + 1. * reachScale));
			} else {
				p.scale(-1., 1.);
			}
			p.translate(-center);
			p.drawEllipse(rect);
			context.st->historyFastShareIcon().paintInCenter(p, rect);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawArc(arcRect, arc::kQuarterLength, spanAngle);
			if (reachRatio) {
				const auto w = style::ConvertFloatScale(kWaveWidth);
				p.setOpacity(ratio - reachRatio);
				p.drawArc(
					arcRect + Margins(reachRatio * reachRatio * w),
					arc::kQuarterLength,
					spanAngle);
			}
		}
		p.restore();
	}
	if (selectionTranslation) {
		p.translate(-selectionTranslation, 0);
	}
	if (selectionModeResult.progress) {
		if (!context.skipSelectionCheck) {
			const auto progress = selectionModeResult.progress;
			if (progress <= 1.) {
				if (context.selected()) {
					if (!_selectionCheckbox) {
						_selectionCheckbox
							= std::make_unique<SelectionCheckbox>(
								[this] { repaintSelectionCheckbox(); });
					}
				}
				if (_selectionCheckbox) {
					_selectionCheckbox->check.setChecked(
						context.selected(),
						anim::type::normal);
				}
				const auto o = ScopedPainterOpacity(p, progress);
				const auto &st = st::msgSelectionCheck;
				const auto right = (delegate()->elementChatMode()
					== ElementChatMode::Wide)
					? std::min(
						int(_bubbleWidthLimit
							+ st::msgPhotoSkip
							+ st::msgSelectionOffset
							+ st::msgPadding.left()
							+ st.size),
						width())
					: width();
				const auto pos = QPoint(
					(right
						- (st::msgSelectionOffset * progress - st.size) / 2
						- st::msgPadding.right() / 2
						- st.size
						- st::historyScroll.deltax),
					rect::bottom(g) - st.size - st::msgSelectionBottomSkip);
				{
					p.setPen(QPen(st.border, st.width));
					p.setBrush(context.st->msgServiceBg());
					auto hq = PainterHighQualityEnabler(p);
					p.drawEllipse(QRect(pos, Size(st.size)));
				}
				if (_selectionCheckbox) {
					const auto painted = _selectionCheckbox->check.paint(
						p,
						pos.x(),
						pos.y(),
						width());
					if (selectionCheckboxCanonical) {
						selectionCheckboxPaintedRect
							= context.mapToElement(p, QRectF(painted));
					}
				}
			} else {
				clearSelectionCheckbox = true;
			}
		}
	} else if (!context.skipSelectionCheck) {
		clearSelectionCheckbox = true;
	}
}

void Message::paintCommentsButton(
		Painter &p,
		QRect &g,
		const PaintContext &context) const {
	if (!data()->repliesAreComments() && !data()->externalReply()) {
		return;
	}
	if (!_comments) {
		_comments = std::make_unique<CommentsButton>();
		history()->owner().registerHeavyViewPart(const_cast<Message*>(this));
	}
	const auto stm = context.messageStyle();
	const auto views = data()->Get<HistoryMessageViews>();

	g.setHeight(g.height() - st::historyCommentsButtonHeight);
	const auto top = g.top() + g.height();
	auto left = g.left();
	auto width = g.width();

	if (_comments->ripple) {
		p.setOpacity(st::historyPollRippleOpacity);
		const auto colorOverride = &stm->msgWaveformInactive->c;
		_comments->ripple->paint(
			p,
			left - _comments->rippleShift,
			top,
			width,
			colorOverride);
		if (_comments->ripple->empty()) {
			_comments->ripple.reset();
		}
		p.setOpacity(1.);
	}

	left += st::historyCommentsSkipLeft;
	width -= st::historyCommentsSkipLeft
		+ st::historyCommentsSkipRight;

	const auto &open = stm->historyCommentsOpen;
	open.paint(p,
		left + width - open.width(),
		top + (st::historyCommentsButtonHeight - open.height()) / 2,
		width);

	if (!views || views->recentRepliers.empty()) {
		const auto &icon = stm->historyComments;
		icon.paint(
			p,
			left,
			top + (st::historyCommentsButtonHeight - icon.height()) / 2,
			width);
		left += icon.width();
	} else {
		auto &list = _comments->userpics;
		const auto limit = HistoryMessageViews::kMaxRecentRepliers;
		const auto count = std::min(int(views->recentRepliers.size()), limit);
		const auto single = st::historyCommentsUserpics.size;
		const auto shift = st::historyCommentsUserpics.shift;
		const auto regenerate = [&] {
			if (list.size() != count) {
				return true;
			}
			for (auto i = 0; i != count; ++i) {
				auto &entry = list[i];
				const auto peer = entry.peer;
				auto &view = entry.view;
				const auto wasView = view.cloud.get();
				if (views->recentRepliers[i] != peer->id
					|| peer->userpicUniqueKey(view) != entry.uniqueKey
					|| view.cloud.get() != wasView) {
					return true;
				}
			}
			return false;
		}();
		if (regenerate) {
			for (auto i = 0; i != count; ++i) {
				const auto peerId = views->recentRepliers[i];
				if (i == list.size()) {
					list.push_back(UserpicInRow{
						history()->owner().peer(peerId)
					});
				} else if (list[i].peer->id != peerId) {
					list[i].peer = history()->owner().peer(peerId);
				}
			}
			while (list.size() > count) {
				list.pop_back();
			}
			GenerateUserpicsInRow(
				_comments->cachedUserpics,
				list,
				st::historyCommentsUserpics,
				limit);
		}
		p.drawImage(
			left,
			top + (st::historyCommentsButtonHeight - single) / 2,
			_comments->cachedUserpics);
		left += single + (count - 1) * (single - shift);
	}

	left += st::historyCommentsSkipText;
	p.setPen(stm->msgFileThumbLinkFg);
	p.setFont(st::semiboldFont);

	const auto textTop = top + (st::historyCommentsButtonHeight - st::semiboldFont->height) / 2;
	p.drawTextLeft(
		left,
		textTop,
		width,
		views ? views->replies.text : tr::lng_replies_view_original(tr::now),
		views ? views->replies.textWidth : -1);

	if (views && data()->areCommentsUnread()) {
		p.setPen(Qt::NoPen);
		p.setBrush(stm->msgFileBg);

		{
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(style::rtlrect(left + views->replies.textWidth + st::mediaUnreadSkip, textTop + st::mediaUnreadTop, st::mediaUnreadSize, st::mediaUnreadSize, width));
		}
	}
}

void Message::paintFromName(
		Painter &p,
		QRect &trect,
		const PaintContext &context,
		std::optional<QRect> *paintedStatusRect) const {
	const auto item = data();
	if (!displayFromName()) {
		return;
	}
	const auto badgeWidth = rightBadgeWidth();
	auto availableLeft = trect.left();
	auto availableWidth = trect.width();
	if (badgeWidth) {
		availableWidth -= st::msgPadding.right() + badgeWidth;
	}

	const auto stm = context.messageStyle();
	const auto from = item->displayFrom();
	const auto info = from ? nullptr : item->displayHiddenSenderInfo();
	Assert(from || info);
	const auto nameFg = FromNameFg(
		context,
		colorIndex(),
		colorCollectible());
	const auto nameText = [&] {
		if (from) {
			validateFromNameText(from);
			return static_cast<const Ui::Text::String*>(&_fromName);
		}
		return &info->nameText();
	}();
	const auto statusWidth = _fromNameStatus
		? st::dialogsPremiumIcon.icon.width()
		: 0;
	const auto nameAvailableWidth = (statusWidth && availableWidth > statusWidth)
		? (availableWidth - statusWidth)
		: availableWidth;
	if (statusWidth && availableWidth > statusWidth) {
		const auto x = availableLeft
			+ std::min(nameAvailableWidth, nameText->maxWidth());
		const auto y = trect.top();
		auto color = nameFg;
		color.setAlpha(115);
		const auto id = from ? from->emojiStatusId() : EmojiStatusId();
		const auto position = QPoint(
			x - 2 * _fromNameStatus->skip,
			y + _fromNameStatus->skip);
		const auto recordStatusRect = [&](QRectF rect) {
			if (paintedStatusRect) {
				*paintedStatusRect = context.mapToElement(p, rect);
			}
		};
		if (_fromNameStatus->id != id) {
			const auto that = const_cast<Message*>(this);
			_fromNameStatus->custom = id
				? MakeWrappedEmoji<Ui::Text::LimitedLoopsEmoji>(
					history()->owner().customEmojiManager().create(
						Data::EmojiStatusCustomId(id),
						[=] { that->repaintFromNameStatus(); }),
					kPlayStatusLimit)
				: nullptr;
			if (id && !_fromNameStatus->id) {
				history()->owner().registerHeavyViewPart(that);
			} else if (!id && _fromNameStatus->id) {
				that->checkHeavyPart();
			}
			_fromNameStatus->id = id;
		}
		if (_fromNameStatus->custom) {
			const auto repaintBounds = _fromNameStatus->custom->paint(p, {
				.textColor = color,
				.now = context.now,
				.position = position,
				.paused = context.paused || On(PowerSaving::kEmojiStatus),
			}).repaintBounds();
			recordStatusRect(repaintBounds);
		} else {
			st::dialogsPremiumIcon.icon.paint(p, x, y, width(), color);
		}
	}
	p.setFont(st::msgNameFont);
	p.setPen(nameFg);
	const auto nameLinkHandler = fromLink();
	const auto nameWidth = std::min(
		nameText->maxWidth(),
		nameAvailableWidth);
	if (!from) {
		if (const auto tooltip = Get<HiddenSenderTooltip>()) {
			tooltip->linkRect = QRect(
				availableLeft,
				trect.top(),
				nameWidth,
				st::msgNameFont->height);
		}
	}
	paintLinkRipple(
		p,
		context,
		nameLinkHandler,
		QRect(availableLeft, trect.top(), nameWidth, st::msgNameFont->height),
		trect.topLeft());
	nameText->draw(p, {
		.position = { availableLeft, trect.top() },
		.availableWidth = nameAvailableWidth,
		.elisionLines = 1,
	});
	const auto skipWidth = nameText->maxWidth()
		+ (_fromNameStatus
			? (st::dialogsPremiumIcon.icon.width()
				+ st::msgServiceFont->spacew)
			: 0)
		+ st::msgServiceFont->spacew;
	availableLeft += skipWidth;
	availableWidth -= skipWidth;

	auto via = item->Get<HistoryMessageVia>();
	if (via && !displayForwardedFrom() && availableWidth > 0) {
		p.setPen(stm->msgServiceFg);
		paintLinkRipple(
			p,
			context,
			via->link,
			QRect(availableLeft, trect.top(), via->width, st::msgServiceFont->height),
			trect.topLeft());
		p.drawText(availableLeft, trect.top() + st::msgServiceFont->ascent, via->text);
		auto skipWidth = via->width + st::msgServiceFont->spacew;
		availableLeft += skipWidth;
		availableWidth -= skipWidth;
	}
	if (const auto guestChat = item->Get<HistoryMessageGuestChat>()) {
		if (availableWidth > 0) {
			p.setPen(stm->msgServiceFg);
			paintLinkRipple(
				p,
				context,
				guestChat->link,
				QRect(availableLeft, trect.top(), guestChat->width, st::msgServiceFont->height),
				trect.topLeft());
			p.drawText(availableLeft, trect.top() + st::msgServiceFont->ascent, guestChat->text);
			auto skipWidth = guestChat->width + st::msgServiceFont->spacew;
			availableLeft += skipWidth;
			availableWidth -= skipWidth;
		}
	}
	if (badgeWidth) {
		p.setPen(stm->msgDateFg);
		if (const auto badge = Get<RightBadge>()) {
			const auto badgeColor = (badge->role == BadgeRole::Creator)
				? st::rankOwnerFg->c
				: (badge->role == BadgeRole::Admin)
				? st::rankAdminFg->c
				: st::rankUserFg->c;
			const auto badgeLeft = trect.left()
				+ trect.width()
				- badge->width;
			if (badge->role != BadgeRole::User) {
				auto bgColor = badgeColor;
				bgColor.setAlphaF(0.15);
				const auto pill = ComputeBadgePillGeometry(badge);
				const auto &padding = st::msgTagBadgePadding;
				const auto badgeTop = trect.top()
					+ (st::msgNameFont->height - pill.height) / 2;
				const auto pillRect = QRect(
					badgeLeft,
					badgeTop,
					pill.width,
					pill.height);
				p.setPen(Qt::NoPen);
				p.setBrush(bgColor);
				{
					auto hq = PainterHighQualityEnabler(p);
					p.drawRoundedRect(
						pillRect,
						pill.height / 2.,
						pill.height / 2.);
				}
				if (badge->ripple) {
					auto rippleColor = badgeColor;
					rippleColor.setAlphaF(0.1);
					badge->ripple->paint(
						p,
						badgeLeft,
						badgeTop,
						width(),
						&rippleColor);
					if (badge->ripple->empty()) {
						badge->ripple.reset();
					}
				}
				p.setPen(badgeColor);
				badge->tag.draw(p, {
					.position = QPoint(
						badgeLeft + (pill.width - pill.textWidth) / 2,
						badgeTop + padding.top()),
					.availableWidth = pill.textWidth,
					.now = context.now,
				});
			} else if (!badge->tag.isEmpty()) {
				if (badge->ripple) {
					const auto pill = ComputeBadgePillGeometry(badge);
					const auto &padding = st::msgTagBadgePadding;
					const auto pillLeft = badgeLeft
						- (pill.width - pill.textWidth) / 2;
					const auto pillTop = trect.top() - padding.top();
					auto rippleColor = badgeColor;
					rippleColor.setAlphaF(0.1);
					badge->ripple->paint(
						p,
						pillLeft,
						pillTop,
						width(),
						&rippleColor);
					if (badge->ripple->empty()) {
						badge->ripple.reset();
					}
				}
				p.setPen(st::rankUserFg);
				badge->tag.draw(p, {
					.position = QPoint(badgeLeft, trect.top()),
					.availableWidth = badge->tag.maxWidth(),
					.now = context.now,
				});
			}
			if (!badge->boosts.isEmpty()) {
				const auto boostWidth = badge->boosts.maxWidth();
				p.setPen(badgeColor);
				badge->boosts.draw(p, {
					.position = QPoint(
						trect.left() + trect.width() - boostWidth,
						trect.top()),
					.availableWidth = boostWidth,
					.now = context.now,
				});
			}
		}
	}
	trect.setY(trect.y() + st::msgNameFont->height);
}

void Message::paintEphemeralBadge(
		Painter &p,
		QRect &trect,
		const PaintContext &context) const {
	const auto badge = Get<EphemeralBadge>();
	if (!badge || badge->text.isEmpty()) {
		return;
	}
	const auto stm = context.messageStyle();
	const auto &icon = stm->historyEphemeralIcon;
	const auto iconTop = trect.y()
		+ (st::msgNameStyle.font->height - icon.height()) / 2;
	icon.paint(p, trect.x(), iconTop, width());
	const auto skip = icon.width() + st::historyEphemeralIconSkip;
	p.setPen(stm->msgServiceFg);
	badge->text.drawLeftElided(
		p,
		trect.x() + skip,
		trect.y(),
		trect.width() - skip,
		width());
	trect.setY(trect.y() + badge->height);
}

void Message::paintTopicButton(
		Painter &p,
		QRect &trect,
		const PaintContext &context) const {
	const auto button = displayedTopicButton();
	if (!button) {
		recordTopicButtonRippleRepaint(p, context, QRect());
		return;
	}
	trect.setTop(trect.top() + st::topicButtonSkip);
	const auto padding = st::topicButtonPadding;
	const auto size = TopicButtonSize(trect.width(), button->name);
	const auto width = size.width();
	const auto height = size.height();
	const auto rect = QRect(trect.topLeft(), size);
	const auto canonical = context.hasElementPainter(p);
	if (canonical
		&& button->ripple
		&& _topicButtonRippleRepaint
		&& _topicButtonRippleRepaint->maskSize != rect.size()) {
		invalidateTopicButtonRippleRepaint();
		clearTopicButtonRipple();
	}
	recordTopicButtonRippleRepaint(p, context, rect);

	const auto stm = context.messageStyle();
	const auto skip = padding.right() + st::topicButtonArrowSkip;
	auto color = stm->msgServiceFg->c;
	color.setAlpha(color.alpha() / 8);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.drawRoundedRect(rect, height / 2, height / 2);
	}
	if (button->ripple) {
		p.save();
		p.translate(rect.topLeft());
		p.setClipRect(
			QRect(QPoint(), rect.size()),
			Qt::IntersectClip);
		button->ripple->paint(
			p,
			0,
			0,
			rect.width(),
			&color);
		p.restore();
		if (canonical && button->ripple->empty()) {
			clearTopicButtonRipple();
		}
	}
	const auto textRect = QRect(
		trect.x() + padding.left(),
		trect.y() + padding.top(),
		width - padding.left() - skip,
		st::msgNameFont->height);
	p.setPen(stm->msgServiceFg);
	p.setTextPalette(stm->fwdTextPalette);
	paintTopicButtonName(p, context, textRect);

	const auto &icon = st::topicButtonArrow;
	icon.paint(
		p,
		rect.x() + rect.width() - skip + st::topicButtonArrowPosition.x(),
		rect.y() + padding.top() + st::topicButtonArrowPosition.y(),
		this->width(),
		stm->msgServiceFg->c);

	trect.setY(trect.y() + height + st::topicButtonSkip);
}

void Message::paintForwardedInfo(
		Painter &p,
		QRect &trect,
		const PaintContext &context) const {
	if (displayForwardedFrom()) {
		const auto item = data();
		const auto st = context.st;
		const auto stm = context.messageStyle();
		const auto forwarded = item->Get<HistoryMessageForwarded>();

		const auto &serviceFont = st::msgServiceFont;
		const auto skip1 = forwarded->psaType.isEmpty()
			? 0
			: st::historyPsaIconSkip1;
		const auto skip2 = forwarded->psaType.isEmpty()
			? 0
			: st::historyPsaIconSkip2;
		const auto fits = (forwarded->text.maxWidth() + skip1 <= trect.width());
		const auto skip = fits ? skip1 : skip2;
		const auto useWidth = trect.width() - skip;
		const auto countedHeight = forwarded->text.countHeight(useWidth);
		const auto breakEverywhere = (countedHeight > 2 * serviceFont->height);
		p.setPen(!forwarded->psaType.isEmpty()
			? st->boxTextFgGood()
			: stm->msgServiceFg);
		p.setFont(serviceFont);
		const auto &fwdPalette = !forwarded->psaType.isEmpty()
			? st->historyPsaForwardPalette()
			: stm->fwdTextPalette;
		const auto rippleLinkRange = (_linkRipple && _linkRipple->link)
			? forwarded->text.linkRangeFor(_linkRipple->link)
			: TextSelection();
		const auto rippleBelongsHere = !rippleLinkRange.empty();
		if (_linkRipple
			&& _linkRipple->ripple
			&& _linkRipple->cachedWidth != useWidth
			&& rippleBelongsHere) {
			_linkRipple = nullptr;
		}
		if (_linkRipple && _linkRipple->ripple && rippleBelongsHere) {
			auto color = p.pen().color();
			color.setAlphaF(0.1);
			const auto position = QPoint(
				trect.x() + _linkRipple->maskOffset.x(),
				trect.y() + _linkRipple->maskOffset.y());
			recordLinkRippleRepaint(p, context, position);
			_linkRipple->ripple->paint(
				p,
				position.x(),
				position.y(),
				width(),
				&color);
			if (_linkRipple->ripple->empty()) {
				_linkRipple = nullptr;
			}
		}
		const auto needRippleMask = _linkRipple
			&& _linkRipple->link
			&& !_linkRipple->ripple
			&& rippleBelongsHere;
		const auto hiddenTooltip = Get<HiddenSenderTooltip>();
		const auto recomputeHidden = hiddenTooltip
			&& (hiddenTooltip->cachedWidth != useWidth);
		const auto hiddenSenderRange = recomputeHidden
			? forwarded->text.linkRangeFor(
				HiddenSenderInfo::ForwardClickHandler())
			: TextSelection();
		auto highlightPath = QPainterPath();
		auto highlightRequest = Ui::Text::HighlightInfoRequest{
			.range = needRippleMask ? rippleLinkRange : hiddenSenderRange,
			.outPath = &highlightPath,
		};
		const auto needHighlight = needRippleMask
			|| !hiddenSenderRange.empty();
		forwarded->text.draw(p, {
			.position = { trect.x(), trect.y() },
			.availableWidth = useWidth,
			.palette = &fwdPalette,
			.paused = p.inactive(),
			.highlight = needHighlight ? &highlightRequest : nullptr,
			.elisionLines = 2,
			.elisionBreakEverywhere = breakEverywhere,
		});
		if (recomputeHidden) {
			hiddenTooltip->cachedWidth = useWidth;
			if (!hiddenSenderRange.empty() && !highlightPath.isEmpty()) {
				hiddenTooltip->linkRect
					= highlightPath.boundingRect().toRect();
			}
		}
		if (needRippleMask && !highlightPath.isEmpty()) {
			createLinkRippleMask(
				highlightPath,
				trect.topLeft(),
				useWidth,
				st::nameRipplePadding,
				st::nameRippleRadius);
		} else if (needRippleMask) {
			_linkRipple = nullptr;
		}
		p.setTextPalette(stm->textPalette);

		if (!forwarded->psaType.isEmpty()) {
			const auto entry = Get<PsaTooltipState>();
			Assert(entry != nullptr);
			const auto shown = entry->buttonVisibleAnimation.value(
				entry->buttonVisible ? 1. : 0.);
			if (shown > 0) {
				const auto &icon = stm->historyPsaIcon;
				const auto position = fits
					? st::historyPsaIconPosition1
					: st::historyPsaIconPosition2;
				const auto x = trect.x() + trect.width() - position.x() - icon.width();
				const auto y = trect.y() + position.y();
				if (shown == 1) {
					icon.paint(p, x, y, trect.width());
				} else {
					p.save();
					p.translate(x + icon.width() / 2, y + icon.height() / 2);
					p.scale(shown, shown);
					p.setOpacity(shown);
					icon.paint(p, -icon.width() / 2, -icon.height() / 2, width());
					p.restore();
				}
			}
		}

		trect.setY(trect.y() + ((fits ? 1 : 2) * serviceFont->height));
	}
}

void Message::paintReplyInfo(
		Painter &p,
		QRect &trect,
		const PaintContext &context) const {
	if (const auto reply = Get<Reply>()) {
		reply->paint(
			p,
			this,
			context,
			trect.x(),
			trect.y(),
			trect.width(),
			true);
		trect.setY(trect.y() + reply->height());
	}
}

void Message::paintSummaryHeaderInfo(
		Painter &p,
		QRect &trect,
		const PaintContext &context) const {
	if (const auto summaryHeader = Get<SummaryHeader>()) {
		summaryHeader->paint(
			p,
			this,
			context,
			trect.x(),
			trect.y(),
			trect.width(),
			true);
		trect.setY(trect.y() + summaryHeader->height());
	}
}

void Message::paintViaBotIdInfo(
		Painter &p,
		QRect &trect,
		const PaintContext &context) const {
	const auto item = data();
	if (!displayFromName() && !displayForwardedFrom()) {
		if (auto via = item->Get<HistoryMessageVia>()) {
			const auto stm = context.messageStyle();
			p.setFont(st::msgServiceNameFont);
			p.setPen(stm->msgServiceFg);
			paintLinkRipple(
				p,
				context,
				via->link,
				QRect(trect.x(), trect.y(), via->width, st::msgServiceNameFont->height),
				trect.topLeft());
			p.drawTextLeft(trect.left(), trect.top(), width(), via->text);
			trect.setY(trect.y() + st::msgServiceNameFont->height);
		}
	}
}

void Message::paintText(
		Painter &p,
		QRect &trect,
		const PaintContext &context) const {
	if (!hasVisibleText()) {
		recordTextRepaintRect(p, context, {});
		return;
	}
	const auto stm = context.messageStyle();
	p.setPen(stm->historyTextFg);
	p.setFont(st::msgFont);
	if (const auto botTop = Get<FakeBotAboutTop>()) {
		botTop->text.drawLeftElided(
			p,
			trect.x(),
			trect.y(),
			trect.width(),
			width());
		trect.setY(trect.y() + botTop->height);
	}
	if (const auto rich = const_cast<Message*>(this)->richpage()) {
		recordTextRepaintRect(p, context, {});
	    paintRichText(p, rich, richPageRect(trect), context);
		return;
	}
	const auto appearing = Get<TextAppearing>();
	const auto appearingClip = appearing && appearing->use;

	if (!context.clip.intersects(trect)
		&& context.skipDrawingParts == PaintContext::SkipDrawingParts::None
		&& !context.gestureHorizontal.translation) {
		return;
	}
	prepareCustomEmojiPaint(p, context, text());

	const auto rippleLinkRange = (_linkRipple && _linkRipple->link)
		? text().linkRangeFor(_linkRipple->link)
		: TextSelection();
	const auto rippleBelongsHere = !rippleLinkRange.empty();
	if (_linkRipple
		&& _linkRipple->ripple
		&& _linkRipple->cachedWidth != trect.width()
		&& rippleBelongsHere) {
		_linkRipple = nullptr;
	}
	if (_linkRipple && _linkRipple->ripple && rippleBelongsHere) {
		auto color = stm->textPalette.linkFg->c;
		color.setAlphaF(0.1);
		const auto position = QPoint(
			trect.x() + _linkRipple->maskOffset.x(),
			trect.y() + _linkRipple->maskOffset.y());
		recordLinkRippleRepaint(p, context, position);
		_linkRipple->ripple->paint(
			p,
			position.x(),
			position.y(),
			width(),
			&color);
		if (_linkRipple->ripple->empty()) {
			_linkRipple = nullptr;
		}
	}
	const auto needRippleMask = _linkRipple
		&& _linkRipple->link
		&& !_linkRipple->ripple
		&& rippleBelongsHere;
	auto ripplePath = QPainterPath();
	auto rippleRequest = Ui::Text::HighlightInfoRequest{
		.range = rippleLinkRange,
		.outPath = &ripplePath,
	};

	auto linePostprocess = std::optional<Ui::Text::LinePostprocess>();
	if (appearingClip) {
		const auto shown = appearing->shownLine;
		const auto &line = appearing->lines[shown];

		p.save();
		p.setClipRect(QRect(
			trect.x(),
			trect.y(),
			trect.width(),
			line.bottom));

		const auto revealedWidth = appearing->revealedLineWidth;
		const auto lineWidth = line.width;
		const auto availableWidth = textRealWidth();
		linePostprocess.emplace(Ui::Text::LinePostprocess{
			.method = [=](int lineIndex) -> Fn<void(QImage&)> {
				if (lineIndex != shown || revealedWidth >= lineWidth) {
					return nullptr;
				}
				return [=](QImage &cache) {
					ApplyRevealGradient(appearing, cache, availableWidth);
				};
			},
			.cache = &appearing->lineCache,
		});
	}

	auto highlightRequest = context.computeHighlightCache();
	auto customEmojiRepaintBounds
		= Ui::Text::CustomEmojiRepaintBounds();
	text().draw(p, {
		.position = trect.topLeft(),
		.availableWidth = std::max(textRealWidth(), trect.width()),
		.palette = &stm->textPalette,
		.pre = stm->preCache.get(),
		.blockquote = context.quoteCache(
			contentColorCollectible(),
			contentColorIndex()),
		.colors = context.st->highlightColors(),
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
		.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
		.selection = context.selection,
		.highlight = needRippleMask
			? &rippleRequest
			: (highlightRequest ? &*highlightRequest : nullptr),
		.useFullWidth = true,
		.linePostprocess = linePostprocess ? &*linePostprocess : nullptr,
		.customEmojiRepaintBounds = &customEmojiRepaintBounds,
	});
	if (needRippleMask && !ripplePath.isEmpty()) {
		createLinkRippleMask(
			ripplePath,
			trect.topLeft(),
			trect.width(),
			st::linkRipplePadding,
			st::linkRippleRadius);
	} else if (needRippleMask) {
		_linkRipple = nullptr;
	}
	const auto needsTextFallback
		= text().hasSpoilers()
		|| !customEmojiRepaintBounds.repaintBoundsKnown;
	auto textAnimationBounds = QRectF();
	if (needsTextFallback || appearingClip) {
		textAnimationBounds = QRectF(
			trect.x(),
			trect.y(),
			std::max(textRealWidth(), trect.width()),
			textHeightFor(trect.width()));
		if (appearingClip) {
			const auto shown = appearing->shownLine;
			const auto bottom = (shown >= 0
				&& shown < int(appearing->lines.size()))
				? appearing->lines[shown].bottom
				: 0;
			textAnimationBounds.setHeight(std::min(
				textAnimationBounds.height(),
				qreal(bottom)));
		}
	}
	if (appearingClip) {
		p.restore();
		customEmojiRepaintBounds.rect
			= customEmojiRepaintBounds.rect.intersected(
				textAnimationBounds);
	}
	recordTextRepaintRect(
		p,
		context,
		customEmojiRepaintBounds,
		needsTextFallback ? textAnimationBounds : QRectF());
}

uint64 Message::recordRichPageRepaintGeometry(
		const Painter &p,
		const PaintContext &context,
		not_null<HistoryMessageRichPage*> rich,
		QRect rect) const {
	const auto generation = SyncRichPageRepaintGeneration(
		rich,
		_richPageRepaintGeneration);
	if (!context.hasElementPainter(p)) {
		return generation;
	}
	auto &geometry = rich->repaintGeometry;
	const auto transform = rect.isEmpty()
		? std::optional<QTransform>()
		: RichPageArticleTransform(p, context, rect.topLeft());
	if (!transform) {
		InvalidateRichPageRepaintGeometry(rich);
		return generation;
	}
	const auto layoutWidth = rich->article.lastLayoutWidth();
	const auto moved = geometry.known
		&& (geometry.articleSize != rect.size()
			|| geometry.layoutWidth != layoutWidth
			|| geometry.articleToElement != *transform);
	auto previous = geometry.stale;
	if (moved) {
		previous = previous.united(
			MapRichPageRect(geometry, geometry.pending));
	}
	geometry.articleToElement = *transform;
	geometry.articleSize = rect.size();
	geometry.layoutWidth = layoutWidth;
	geometry.known = true;
	const auto current = (moved || !geometry.stale.isEmpty())
		? MapRichPageRect(geometry, geometry.pending)
		: QRect();
	geometry.pending = QRect();
	geometry.stale = QRect();
	const auto damage = previous.united(current);
	if (!damage.isEmpty()) {
		repaint(damage);
	}
	return generation;
}

void Message::paintRichText(
		Painter &p,
		not_null<HistoryMessageRichPage*> rich,
		QRect rect,
		const PaintContext &context) const {
	const auto repaintGeneration = recordRichPageRepaintGeometry(
		p,
		context,
		rich,
		rect);
	auto repaintRect = Fn<void(QRect)>();
	if (context.hasElementPainter(p)) {
		repaintRect = [
				weak = base::make_weak(const_cast<Message*>(this)),
				repaintGeneration](QRect articleRect) {
			if (const auto owner = weak.get()) {
				owner->requestRichPageRepaint(
					articleRect,
					repaintGeneration);
			}
		};
	}
	const auto pathShiftGradientPainted = Fn<void(QRect)>(
		[&, this](QRect articleRect) {
			delegate()->elementPathShiftGradientPainted(
				this,
				p,
				context,
				articleRect);
		});
	const auto stm = context.messageStyle();
	const auto paletteVersion = context.st->paletteVersion();
	if (rich->paletteVersion != paletteVersion) {
		rich->paletteVersion = paletteVersion;
		rich->article.invalidatePaletteCache();
	}
	const auto appearing = Get<TextAppearing>();
	const auto appearingClip = appearing
		&& appearing->use
		&& (appearing->shownLine < int(appearing->lines.size()));
	const auto viewportClip = QRect(
		QPoint(),
		rect.size()
	).intersected(context.clip.translated(-rect.topLeft()));
	auto articleClip = viewportClip;
	if (appearingClip) {
		const auto &line = appearing->lines[appearing->shownLine];
		const auto shownHeight = std::min(
			std::max(appearing->shownHeight, 0),
			std::max(line.bottom, 0));
		articleClip = articleClip.intersected(QRect(
			0,
			0,
			rect.width(),
			shownHeight));
	}
	rich->article.setVisibleTopBottom(
		std::clamp(viewportClip.top(), 0, rect.height()),
		std::clamp(viewportClip.bottom() + 1, 0, rect.height()));
	auto articleContext = Iv::Markdown::MarkdownArticlePaintContext(
		context).translated(-rect.topLeft());
	if (context.messageSelection && context.messageSelection->isRichPage()) {
		articleContext.selectionState.selection
			= context.messageSelection->richPage.selection;
		articleContext.selectionState.endpoints
			= &context.messageSelection->richPage.endpoints;
	}
	articleContext.clip = articleClip;
	articleContext.caches = {
		.pre = stm->preCache.get(),
		.blockquote = context.quoteCache(
			contentColorCollectible(),
			contentColorIndex()),
		.thinking = &rich->thinkingPaintCache,
		.pathShiftGradient = delegate()->elementPathShiftGradient().get(),
		.pathShiftGradientPainted = &pathShiftGradientPainted,
		.colors = context.st->highlightColors(),
		.st = &stm->richPageStyle,
		.repaint = [
				weak = base::make_weak(const_cast<Message*>(this)),
				repaintGeneration] {
			if (const auto owner = weak.get()) {
				owner->requestRichPageRepaint(QRect(), repaintGeneration);
			}
		},
		.repaintRect = std::move(repaintRect),
	};
	auto revealPostprocess
		= std::optional<Iv::Markdown::MarkdownArticleRevealPostprocess>();
	auto revealState
		= std::optional<Iv::Markdown::MarkdownArticleRevealPaintState>();
	if (appearingClip) {
		revealPostprocess.emplace(
			Iv::Markdown::MarkdownArticleRevealPostprocess{
				.method = [=](
						int lineIndex,
						int availableWidth) -> Fn<void(QImage&)> {
					if (lineIndex != appearing->shownLine
						|| appearing->revealedLineWidth
							>= appearing->lines[lineIndex].width) {
						return nullptr;
					}
					return [=](QImage &cache) {
						ApplyRevealGradient(
							appearing,
							cache,
							availableWidth);
					};
				},
				.cache = &appearing->lineCache,
			});
		revealState.emplace(Iv::Markdown::MarkdownArticleRevealPaintState{
			.activeLine = appearing->shownLine,
			.nextLine = 0,
			.postprocess = &*revealPostprocess,
		});
		articleContext.reveal = &*revealState;
	}
	p.save();
	p.translate(rect.topLeft());
	rich->article.paint(p, articleContext);
	p.restore();
}

PointState Message::pointState(QPoint point) const {
	auto g = countGeometry();
	if (g.width() < 1 || isHidden()) {
		return PointState::Outside;
	}

	const auto media = this->media();
	const auto item = data();
	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	if (drawBubble()) {
		if (!g.contains(point)) {
			return PointState::Outside;
		}
		if (const auto mediaDisplayed = media && media->isDisplayed()) {
			// Hack for grouped media point state.
			const auto entry = logEntryOriginal();
			const auto check = factcheckBlock();

			// Entry page is always a bubble bottom.
			auto mediaOnBottom = (mediaDisplayed && media->isBubbleBottom()) || check || (entry/* && entry->isBubbleBottom()*/);
			auto mediaOnTop = (mediaDisplayed && media->isBubbleTop()) || (entry && entry->isBubbleTop());

			if (item->repliesAreComments() || item->externalReply()) {
				g.setHeight(g.height() - st::historyCommentsButtonHeight);
			}

			auto trect = g.marginsRemoved(st::msgPadding);
			if (reactionsInBubble) {
				const auto reactionsHeight = (_viewButton ? 0 : st::mediaInBubbleSkip)
					+ _reactions->height();
				trect.setHeight(trect.height() - reactionsHeight);
			}
			if (_viewButton) {
				trect.setHeight(trect.height() - _viewButton->height());
				if (reactionsInBubble) {
					trect.setHeight(trect.height() + st::msgPadding.bottom());
				} else if (mediaDisplayed) {
					trect.setHeight(trect.height() - st::mediaInBubbleSkip);
				}
			}
			if (mediaOnBottom) {
				trect.setHeight(trect.height() + st::msgPadding.bottom());
			}
			//if (mediaOnTop) {
			//	trect.setY(trect.y() - st::msgPadding.top());
			//} else {
			//	if (getStateFromName(point, trect, &result)) return result;
			//	if (getStateTopicButton(point, trect, &result)) return result;
			//	if (getStateForwardedInfo(point, trect, &result, request)) return result;
			//	if (getStateReplyInfo(point, trect, &result)) return result;
			//	if (getStateViaBotIdInfo(point, trect, &result)) return result;
			//}
			if (check) {
				auto checkHeight = check->height();
				trect.setHeight(trect.height() - checkHeight - st::mediaInBubbleSkip);
			}
			if (entry) {
				auto entryHeight = entry->height();
				trect.setHeight(trect.height() - entryHeight);
			}

			const auto mediaHeight = mediaDisplayed ? media->height() : 0;
			const auto mediaLeft = trect.x() - st::msgPadding.left();
			const auto mediaTop = (!mediaDisplayed || _invertMedia)
				? (trect.y() + (mediaOnTop ? 0 : st::mediaInBubbleSkip))
				: (trect.y() + trect.height() - mediaHeight);
			if (mediaDisplayed && _invertMedia) {
				trect.setY(mediaTop
					+ mediaHeight
					+ (mediaOnBottom ? 0 : st::mediaInBubbleSkip));
			}
			if (point.y() >= mediaTop
				&& point.y() < mediaTop + mediaHeight) {
				return media->pointState(point - QPoint(mediaLeft, mediaTop));
			}
		}
		return PointState::Inside;
	} else if (media) {
		return media->pointState(point - g.topLeft());
	}
	return PointState::Outside;
}

bool Message::displayFromPhoto() const {
	return hasFromPhoto() && !isAttachedToNext();
}

void Message::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	const auto startLinkRipple = [&] {
		if (!_linkRipple) {
			if (!pressed) {
				return;
			}
			_linkRipple = std::make_unique<LinkRipple>();
		}
		_linkRipple->link = handler;
		toggleLinkRipple(pressed);
	};
	if (const auto markup = data()->Get<HistoryMessageReplyMarkup>()) {
		if (const auto keyboard = markup->inlineKeyboard.get()) {
			keyboard->clickHandlerPressedChanged(
				handler,
				pressed,
				countMessageRounding());
		}
	}
	Element::clickHandlerPressedChanged(handler, pressed);
	if (const auto check = factcheckBlock()) {
		check->clickHandlerPressedChanged(handler, pressed);
	}
	if (!handler) {
		return;
	} else if (_rightAction && (handler == _rightAction->link)) {
		toggleRightActionRipple(pressed);
	} else if (_rightAction
		&& _rightAction->second
		&& (handler == _rightAction->second->link)) {
		const auto rightSize = rightActionSize();
		Assert(rightSize != std::nullopt);
		if (pressed) {
			if (!_rightAction->second->ripple) {
				// Create a ripple.
				_rightAction->second->ripple
					= std::make_unique<Ui::RippleAnimation>(
						st::defaultRippleAnimation,
						Ui::RippleAnimation::RoundRectMask(
							Size(rightSize->width()),
							rightSize->width() / 2),
						[=] { repaint(); });
			}
			_rightAction->second->ripple->add(_rightAction->lastPoint);
		} else if (_rightAction->second->ripple) {
			_rightAction->second->ripple->lastStop();
		}
	} else if (_comments && (handler == _comments->link)) {
		toggleCommentsButtonRipple(pressed);
	} else if (_topicButton && (handler == _topicButton->link)) {
		toggleTopicButtonRipple(pressed);
	} else if (_viewButton) {
		_viewButton->checkLink(handler, pressed);
	} else if (const auto reply = Get<Reply>()
		; reply && (handler == reply->link())) {
		toggleReplyRipple(pressed);
	} else if (const auto summaryHeader = Get<SummaryHeader>()
		; summaryHeader && (handler == summaryHeader->link())) {
		toggleSummaryHeaderRipple(pressed);
	} else if (_summarize && (handler == _summarize->link())) {
		if (pressed) {
			_summarize->addRipple();
		} else {
			_summarize->stopRipple();
		}
	} else if (const auto badge = Get<RightBadge>()
		; badge && badge->tagLink && handler == badge->tagLink) {
		toggleBadgeRipple(pressed);
	} else if (displayFromName() && handler == fromLink()) {
		if (_fromLinkRipplePointSet || !pressed) {
			startLinkRipple();
		}
	} else if (const auto via = data()->Get<HistoryMessageVia>()
		; via
		&& (handler == via->link)
		&& !displayForwardedFrom()) {
		startLinkRipple();
	} else if (const auto guestChat = data()->Get<HistoryMessageGuestChat>()
		; guestChat
		&& (handler == guestChat->link)) {
		startLinkRipple();
	} else if (const auto forwarded = data()->Get<HistoryMessageForwarded>()
		; forwarded
		&& displayForwardedFrom()
		&& !forwarded->text.linkRangeFor(handler).empty()) {
		startLinkRipple();
	} else if (hasVisibleText()
		&& IsRippleLink(handler)
		&& !text().linkRangeFor(handler).empty()) {
		startLinkRipple();
	} else if (const auto rich = richpage()
		; rich
		&& ((handler == rich->handler)
			|| (handler == rich->handlerHorizontalScrollPressed))) {
		if (pressed) {
			if ((handler == rich->handler)
				&& rich->handlerHorizontalScrollHit
				&& rich->article.beginHorizontalScroll(
					rich->handlerHorizontalScrollPoint,
					false)) {
				rich->handlerHorizontalScrollActive = true;
				rich->handlerHorizontalScrollPressed = handler;
			}
		} else if (rich->handlerHorizontalScrollActive
			&& (handler == rich->handlerHorizontalScrollPressed)) {
			rich->article.endHorizontalScroll();
			rich->handlerHorizontalScrollActive = false;
			rich->handlerHorizontalScrollPressed = nullptr;
		}
		if ((handler == rich->handler) && rich->handlerPlaceholderId) {
			if (pressed) {
				rich->article.addPlaceholderRipple(
					rich->handlerPlaceholderId,
					rich->handlerPlaceholderPoint);
			} else {
				rich->article.stopPlaceholderRipple(rich->handlerPlaceholderId);
			}
		}
	} else if (_reactions) {
		_reactions->clickHandlerPressedChanged(handler, pressed);
	}
}

void Message::toggleCommentsButtonRipple(bool pressed) {
	Expects(_comments != nullptr);

	if (!drawBubble()) {
		return;
	} else if (pressed) {
		if (!_comments->ripple) {
			createCommentsButtonRipple();
		}
		_comments->ripple->add(_comments->lastPoint
			+ QPoint(_comments->rippleShift, 0));
	} else if (_comments->ripple) {
		_comments->ripple->lastStop();
	}
}

void Message::toggleRightActionRipple(bool pressed) {
	Expects(_rightAction != nullptr);

	const auto rightSize = rightActionSize();
	Assert(rightSize != std::nullopt);

	if (pressed) {
		if (!_rightAction->ripple) {
			// Create a ripple.
			const auto size = _rightAction->second
				? Size(rightSize->width())
				: *rightSize;
			_rightAction->ripple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				Ui::RippleAnimation::RoundRectMask(size, size.width() / 2),
				[=] { repaint(); });
		}
		_rightAction->ripple->add(_rightAction->lastPoint);
	} else if (_rightAction->ripple) {
		_rightAction->ripple->lastStop();
	}
}

void Message::toggleBadgeRipple(bool pressed) {
	const auto badge = Get<RightBadge>();
	if (!badge) {
		return;
	} else if (pressed) {
		if (!badge->ripple) {
			const auto pill = ComputeBadgePillGeometry(badge);
			auto mask = Ui::RippleAnimation::RoundRectMask(
				QSize(pill.width, pill.height),
				pill.height / 2);
			badge->ripple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				std::move(mask),
				[=] { repaint(); });
		}
		badge->ripple->add(badge->lastPoint);
	} else if (badge->ripple) {
		badge->ripple->lastStop();
	}
}

void Message::toggleReplyRipple(bool pressed) {
	const auto reply = Get<Reply>();
	if (!reply) {
		return;
	}

	if (pressed) {
		if (!unwrapped()) {
			const auto &padding = st::msgPadding;
			const auto geometry = countGeometry();
			const auto margins = reply->margins();
			const auto size = QSize(
				geometry.width() - padding.left() - padding.right(),
				reply->height() - margins.top() - margins.bottom());
			reply->createRippleAnimation(this, size);
		}
		reply->addRipple();
	} else {
		reply->stopLastRipple();
	}
}

void Message::toggleSummaryHeaderRipple(bool pressed) {
	const auto summaryHeader = Get<SummaryHeader>();
	if (!summaryHeader) {
		return;
	}

	if (pressed) {
		if (!unwrapped()) {
			const auto size = QSize(
				countGeometry().width() - rect::m::sum::h(st::msgPadding),
				summaryHeader->height()
					- rect::m::sum::v(summaryHeader->margins()));
			summaryHeader->createRippleAnimation(this, size);
		}
		summaryHeader->addRipple();
	} else {
		summaryHeader->stopLastRipple();
	}
}

BottomRippleMask Message::bottomRippleMask(int buttonHeight) const {
	using namespace Ui;
	using namespace Images;
	using Radius = CachedCornerRadius;
	using Corner = BubbleCornerRounding;
	const auto g = countGeometry();
	const auto buttonWidth = g.width();
	const auto &large = CachedCornersMasks(Radius::BubbleLarge);
	const auto &small = CachedCornersMasks(Radius::BubbleSmall);
	const auto rounding = countBubbleRounding();
	const auto icon = (rounding.bottomLeft == Corner::Tail)
		? &st::historyBubbleTailInLeft
		: (rounding.bottomRight == Corner::Tail)
		? &st::historyBubbleTailInRight
		: nullptr;
	const auto shift = (rounding.bottomLeft == Corner::Tail)
		? icon->width()
		: 0;
	const auto added = shift ? shift : icon ? icon->width() : 0;
	auto corners = CornersMaskRef();
	const auto set = [&](int index) {
		corners.p[index] = (rounding[index] == Corner::Large)
			? &large[index]
			: (rounding[index] == Corner::Small)
			? &small[index]
			: nullptr;
	};
	set(kBottomLeft);
	set(kBottomRight);
	const auto drawer = [&](QPainter &p) {
		p.setCompositionMode(QPainter::CompositionMode_Source);
		const auto ratio = style::DevicePixelRatio();
		const auto corner = [&](int index, bool right) {
			if (const auto image = corners.p[index]) {
				const auto width = image->width() / ratio;
				const auto height = image->height() / ratio;
				p.drawImage(
					QRect(
						shift + (right ? (buttonWidth - width) : 0),
						buttonHeight - height,
						width,
						height),
					*image);
			}
		};
		corner(kBottomLeft, false);
		corner(kBottomRight, true);
		if (icon) {
			const auto left = shift ? 0 : buttonWidth;
			p.fillRect(
				QRect{ left, 0, added, buttonHeight },
				Qt::transparent);
			icon->paint(
				p,
				left,
				buttonHeight - icon->height(),
				buttonWidth + shift,
				Qt::white);
		}
	};
	return {
		RippleAnimation::MaskByDrawer(
			QSize(buttonWidth + added, buttonHeight),
			true,
			drawer),
		shift,
	};
}

void Message::createCommentsButtonRipple() {
	auto mask = bottomRippleMask(st::historyCommentsButtonHeight);
	_comments->ripple = std::make_unique<Ui::RippleAnimation>(
		st::defaultRippleAnimation,
		std::move(mask.image),
		[=] { repaint(); });
	_comments->rippleShift = mask.shift;
}

void Message::toggleTopicButtonRipple(bool pressed) {
	Expects(_topicButton != nullptr);

	if (!drawBubble()) {
		return;
	} else if (pressed) {
		if (!_topicButton->ripple) {
			createTopicButtonRipple();
		}
		_topicButton->ripple->add(_topicButton->lastPoint);
	} else if (_topicButton->ripple) {
		_topicButton->ripple->lastStop();
	}
}

void Message::paintLinkRipple(
		Painter &p,
		const PaintContext &context,
		const ClickHandlerPtr &handler,
		QRect linkRect,
		QPoint textPosition) const {
	const auto raw = _linkRipple.get();
	if (!raw || raw->link != handler) {
		return;
	}
	if (const auto ripple = raw->ripple.get()) {
		auto color = p.pen().color();
		color.setAlpha(25);
		const auto position = textPosition + raw->maskOffset;
		recordLinkRippleRepaint(p, context, position);
		ripple->paint(
			p,
			position.x(),
			position.y(),
			width(),
			&color);
		if (ripple->empty()) {
			_linkRipple = nullptr;
		}
	} else {
		createLinkRippleMask(
			linkRect,
			textPosition,
			st::nameRipplePadding,
			st::nameRippleRadius);
	}
}

void Message::recordLinkRippleRepaint(
		const Painter &p,
		const PaintContext &context,
		QPoint position) const {
	const auto ripple = _linkRipple.get();
	if (!ripple
		|| !ripple->ripple
		|| ripple->maskSize.isEmpty()
		|| !context.hasElementPainter(p)) {
		return;
	}
	const auto rect = style::rtlrect(
		QRect(position, ripple->maskSize),
		width());
	const auto mapped = context.mapToElement(p, QRectF(rect));
	const auto known = mapped.has_value();
	const auto current = known ? *mapped : QRect();
	const auto stale = base::take(ripple->stale);
	const auto previous = stale.united(base::take(ripple->current));
	ripple->pending = false;
	ripple->current = current;
	ripple->known = known;
	if (!known) {
		ripple->stale = previous;
		if (!previous.isEmpty()) {
			ripple->pending = true;
			Ui::LogUnknownGeometryRepaint("message link ripple");
			repaint();
		}
	} else if (!previous.isEmpty()
		&& (!stale.isEmpty() || previous != current)) {
		ripple->pending = true;
		repaint(previous.united(current));
	}
}

void Message::repaintLinkRipple(uint64 generation) const {
	const auto ripple = _linkRipple.get();
	if (!ripple
		|| !ripple->ripple
		|| ripple->generation != generation
		|| ripple->pending
		|| (ripple->known && ripple->current.isEmpty())) {
		return;
	}
	ripple->pending = true;
	if (ripple->known) {
		repaint(ripple->current);
	} else {
		Ui::LogUnknownGeometryRepaint("message link ripple");
		repaint();
	}
}

uint64 Message::resetLinkRippleRepaint(QSize maskSize) const {
	Expects(_linkRipple != nullptr);

	auto &ripple = *_linkRipple;
	ripple.stale = ripple.stale.united(base::take(ripple.current));
	ripple.maskSize = maskSize;
	ripple.pending = false;
	ripple.known = false;
	return ++ripple.generation;
}

void Message::toggleLinkRipple(bool pressed) {
	if (!drawBubble()) {
		return;
	} else if (pressed) {
		repaint();
	} else if (const auto ripple = _linkRipple
		? _linkRipple->ripple.get()
		: nullptr) {
		ripple->lastStop();
	}
}

void Message::recordLinkRipplePoint(
		QPoint point,
		QPoint textOrigin) const {
	_linkRippleLastPoint = point - textOrigin;
}

void Message::createLinkRippleMask(
		const QPainterPath &path,
		QPoint textPosition,
		int useWidth,
		style::margins padding,
		int radius) const {
	auto rects = std::vector<QRect>();
	for (const auto &polygon : path.toSubpathPolygons()) {
		rects.push_back(polygon.boundingRect().toAlignedRect());
	}
	auto boundingRect = QRect();
	for (auto &rect : rects) {
		rect = rect.marginsAdded(padding);
		if (boundingRect.isEmpty()) {
			boundingRect = rect;
		} else {
			boundingRect = boundingRect.united(rect);
		}
	}
	if (boundingRect.isEmpty()) {
		return;
	}
	const auto topLeft = boundingRect.topLeft();
	const auto maskOrigin = topLeft - textPosition;
	auto mask = Ui::RippleAnimation::MaskByDrawer(
		boundingRect.size(),
		false,
		[&](QPainter &p) {
			for (const auto &rect : rects) {
				const auto shifted = rect.translated(-topLeft);
				p.drawRoundedRect(shifted, radius, radius);
			}
		});
	_linkRipple->maskOffset = maskOrigin;
	_linkRipple->cachedWidth = useWidth;
	const auto weak = base::make_weak(this);
	const auto generation = resetLinkRippleRepaint(boundingRect.size());
	_linkRipple->ripple = std::make_unique<Ui::RippleAnimation>(
		st::defaultRippleAnimation,
		std::move(mask),
		[weak, generation] {
			if (const auto strong = weak.get()) {
				strong->repaintLinkRipple(generation);
			}
		});
	_linkRipple->ripple->add(_linkRippleLastPoint - maskOrigin);
}

void Message::createLinkRippleMask(
		QRect linkRect,
		QPoint textPosition,
		style::margins padding,
		int radius) const {
	auto rect = linkRect.marginsAdded(padding);
	const auto maskOrigin = rect.topLeft() - textPosition;
	const auto size = rect.size();
	auto mask = Ui::RippleAnimation::MaskByDrawer(
		size,
		false,
		[&](QPainter &p) {
			p.drawRoundedRect(QRect(QPoint(), size), radius, radius);
		});
	_linkRipple->maskOffset = maskOrigin;
	_linkRipple->cachedWidth = 0;
	const auto weak = base::make_weak(this);
	const auto generation = resetLinkRippleRepaint(size);
	_linkRipple->ripple = std::make_unique<Ui::RippleAnimation>(
		st::defaultRippleAnimation,
		std::move(mask),
		[weak, generation] {
			if (const auto strong = weak.get()) {
				strong->repaintLinkRipple(generation);
			}
		});
	_linkRipple->ripple->add(_linkRippleLastPoint - maskOrigin);
}

void Message::createTopicButtonRipple() {
	const auto geometry = countGeometry().marginsRemoved(st::msgPadding);
	const auto size = TopicButtonSize(geometry.width(), _topicButton->name);
	auto mask = Ui::RippleAnimation::RoundRectMask(
		size,
		size.height() / 2);
	const auto weak = base::make_weak(this);
	const auto generation = resetTopicButtonRippleRepaint();
	_topicButtonRippleRepaint->maskSize = size;
	_topicButton->ripple = std::make_unique<Ui::RippleAnimation>(
		st::defaultRippleAnimation,
		std::move(mask),
		[weak, generation] {
			if (const auto strong = weak.get()) {
				strong->repaintTopicButtonRipple(generation);
			}
		});
}

bool Message::hasHeavyPart() const {
	return _comments
		|| (_fromNameStatus && _fromNameStatus->custom)
		|| Element::hasHeavyPart();
}

void Message::unloadHeavyPart() {
	Element::unloadHeavyPart();
	_comments = nullptr;
	if (_topicButton) {
		invalidateTopicButtonNameRepaint();
		_topicButton->name.unloadPersistentAnimation();
	}
	if (_fromNameStatus) {
		_fromNameStatus->custom = nullptr;
		_fromNameStatus->id = EmojiStatusId();
		_fromNameStatus->lastRepaintRect = std::nullopt;
	}
	if (const auto summaryHeader = Get<SummaryHeader>()) {
		summaryHeader->unloadHeavyPart();
	}
}

bool Message::hasFromPhoto() const {
	if (isHidden()) {
		return false;
	}
	switch (context()) {
	case Context::AdminLog:
		return true;
	case Context::Monoforum:
		return (delegate()->elementChatMode() == ElementChatMode::Wide);
	case Context::History:
	case Context::ChatPreview:
	case Context::TTLViewer:
	case Context::Pinned:
	case Context::Replies:
	case Context::SavedSublist:
	case Context::ScheduledTopic: {
		const auto item = data();
		if (item->isSponsored()) {
			return false;
		} else if (item->isPostHidingAuthor()) {
			return false;
		} else if (item->isPost()) {
			return true;
		} else if (item->isEmpty()
			|| item->isFakeAboutView()
			|| isCommentsRootView()) {
			return false;
		}
		const auto mode = delegate()->elementChatMode();
		if (mode != ElementChatMode::Default) {
			return (mode == ElementChatMode::Wide);
		} else if (item->history()->peer->isVerifyCodes()) {
			return !hasOutLayout();
		} else if (item->Has<HistoryMessageForwarded>()) {
			const auto peer = item->history()->peer;
			if (peer->isSelf() || peer->isRepliesChat()) {
				return !hasOutLayout();
			}
		}
		if (item->isGuestChatBotMessage()) {
			return true;
		}
		return !item->out() && !item->history()->peer->isUser();
	} break;
	case Context::ContactPreview:
	case Context::ShortcutMessages:
		return false;
	}
	Unexpected("Context in Message::hasFromPhoto.");
}

TextState Message::textState(
		QPoint point,
		StateRequest request) const {
	_fromLinkRipplePointSet = 0;

	const auto item = data();
	const auto media = this->media();

	auto result = TextState(item);
	const auto visibleMediaTextLen = visibleMediaTextLength();
	const auto visibleTextLen = visibleTextLength();
	const auto minSymbol = (_invertMedia && request.onlyMessageText)
		? visibleMediaTextLen
		: 0;
	SetTextStatePosition(&result, minSymbol, false);

	auto g = countGeometry();
	if (g.width() < 1 || isHidden()) {
		return result;
	}

	if (const auto service = Get<ServicePreMessage>()) {
		result.link = service->textState(point, request, g);
		if (result.link) {
			return result;
		}
	}

	const auto bubble = drawBubble();
	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	const auto mediaDisplayed = media && media->isDisplayed();

	if (_reactions && !reactionsInBubble) {
		const auto reactionsHeight = st::mediaInBubbleSkip + _reactions->height();
		const auto reactionsLeft = (!bubble && mediaDisplayed)
			? media->contentRectForReactions().x()
			: 0;
		g.setHeight(g.height() - reactionsHeight);
		const auto reactionsPosition = QPoint(reactionsLeft + g.left(), g.top() + g.height() + st::mediaInBubbleSkip);
		if (_reactions->getState(point - reactionsPosition, &result)) {
			AddTextStateOffset(&result, visibleMediaTextLen + visibleTextLen);
			return result;
		}
	}

	const auto keyboard = item->inlineReplyKeyboard();
	auto keyboardHeight = 0;
	if (keyboard) {
		keyboardHeight = keyboard->naturalHeight();
		g.setHeight(g.height() - st::msgBotKbButton.margin - keyboardHeight);

		if (item->isHistoryEntry() || item->isAdminLogEntry()) {
			const auto keyboardPosition = QPoint(g.left(), g.top() + g.height() + st::msgBotKbButton.margin);
			if (QRect(keyboardPosition, QSize(g.width(), keyboardHeight)).contains(point)) {
				AddTextStateOffset(&result, visibleMediaTextLen + visibleTextLen);
				result.link = keyboard->getLink(point - keyboardPosition);
				return result;
			}
		}
	}

	if (bubble) {
		const auto inBubble = g.contains(point);
		const auto check = factcheckBlock();
		const auto entry = logEntryOriginal();

		// Entry page is always a bubble bottom.
		auto mediaOnBottom = (mediaDisplayed && media->isBubbleBottom()) || check || (entry/* && entry->isBubbleBottom()*/);
		auto mediaOnTop = (mediaDisplayed && media->isBubbleTop()) || (entry && entry->isBubbleTop());

		auto inner = g;
		if (getStateCommentsButton(point, inner, &result)) {
			AddTextStateOffset(&result, visibleMediaTextLen + visibleTextLen);
			return result;
		}
		auto trect = inner.marginsRemoved(st::msgPadding);
		const auto additionalInfoSkip = (mediaDisplayed
			&& !media->additionalInfoString().isEmpty())
			? st::msgDateFont->height
			: 0;
		const auto reactionsTop = (reactionsInBubble && !_viewButton)
			? (additionalInfoSkip + st::mediaInBubbleSkip)
			: additionalInfoSkip;
		const auto reactionsHeight = reactionsInBubble
			? (reactionsTop + _reactions->height())
			: 0;
		if (reactionsInBubble) {
			trect.setHeight(trect.height() - reactionsHeight);
			const auto reactionsPosition = QPoint(trect.left(), trect.top() + trect.height() + reactionsTop);
			if (_reactions->getState(point - reactionsPosition, &result)) {
				AddTextStateOffset(&result, visibleMediaTextLen + visibleTextLen);
				return result;
			}
		}
		if (_viewButton) {
			const auto belowInfo = _viewButton->belowMessageInfo();
			const auto infoHeight = reactionsInBubble
				? (reactionsHeight + 2 * st::mediaInBubbleSkip)
				: _bottomInfo.height();
			const auto heightMargins = QMargins(0, 0, 0, infoHeight);
			if (_viewButton->getState(
					point,
					_viewButton->countRect(belowInfo
						? inner
						: inner - heightMargins),
					&result)) {
				AddTextStateOffset(&result, visibleMediaTextLen + visibleTextLen);
				return result;
			}
			if (belowInfo) {
				inner.setHeight(inner.height() - _viewButton->height());
			}
			trect.setHeight(trect.height() - _viewButton->height());
			if (reactionsInBubble) {
				trect.setHeight(trect.height() - st::mediaInBubbleSkip + st::msgPadding.bottom());
			} else if (mediaDisplayed) {
				trect.setHeight(trect.height() - st::mediaInBubbleSkip);
			}
		}
		if (mediaOnBottom) {
			trect.setHeight(trect.height() + st::msgPadding.bottom());
		}
		if (mediaOnTop) {
			trect.setY(trect.y() - st::msgPadding.top());
		} else if (inBubble) {
			if (getStateFromName(point, trect, &result)) {
				return result;
			}
			if (const auto badge = Get<EphemeralBadge>()) {
				trect.setTop(trect.top() + badge->height);
			}
			if (getStateTopicButton(point, trect, &result)) {
				return result;
			}
			if (getStateForwardedInfo(point, trect, &result, request)) {
				return result;
			}
			if (getStateViaBotIdInfo(point, trect, &result)) {
				return result;
			}
			if (getStateReplyInfo(point, trect, &result)) {
				return result;
			}
			if (getStateSummaryHeaderInfo(point, trect, &result)) {
				return result;
			}
		}
		if (entry) {
			auto entryHeight = entry->height();
			trect.setHeight(trect.height() - entryHeight);
			auto entryLeft = inner.left();
			auto entryTop = trect.y() + trect.height();
			if (point.y() >= entryTop && point.y() < entryTop + entryHeight) {
				result = entry->textState(
					point - QPoint(entryLeft, entryTop),
					request);
				AddTextStateOffset(
					&result,
					visibleTextLength() + visibleMediaTextLength());
			}
		}
		if (check) {
			auto checkHeight = check->height();
			trect.setHeight(trect.height() - checkHeight - st::mediaInBubbleSkip);
			auto checkLeft = inner.left();
			auto checkTop = trect.y() + trect.height() + st::mediaInBubbleSkip;
			if (point.y() >= checkTop && point.y() < checkTop + checkHeight) {
				result = check->textState(
					point - QPoint(checkLeft, checkTop),
					request);
				AddTextStateOffset(
					&result,
					visibleTextLength() + visibleMediaTextLength());
			}
		}

		auto checkBottomInfoState = [&] {
			if (mediaOnBottom
				&& (check || entry || media->customInfoLayout())) {
				return;
			}
			const auto bottomInfoResult = bottomInfoTextState(
				inner.left() + inner.width(),
				inner.top() + inner.height(),
				point,
				InfoDisplayType::Default);
			if (bottomInfoResult.link
				|| bottomInfoResult.cursor != CursorState::None
				|| bottomInfoResult.customTooltip) {
				result = bottomInfoResult;
			}
		};
		if (!inBubble) {
			if (point.y() >= g.y() + g.height()) {
				AddTextStateOffset(&result, visibleTextLen + visibleMediaTextLen);
			}
		} else if (result.symbol <= minSymbol) {
			const auto mediaHeight = mediaDisplayed ? media->height() : 0;
			const auto mediaLeft = trect.x() - st::msgPadding.left();
			const auto mediaTop = (!mediaDisplayed || _invertMedia)
				? (trect.y() + (mediaOnTop ? 0 : st::mediaInBubbleSkip))
				: (trect.y() + trect.height() - mediaHeight);
			if (mediaDisplayed && _invertMedia) {
				trect.setY(mediaTop
					+ mediaHeight
					+ (mediaOnBottom ? 0 : st::mediaInBubbleSkip));
			}
			if (point.y() >= mediaTop
				&& point.y() < mediaTop + mediaHeight) {
				result = media->textState(
					point - QPoint(mediaLeft, mediaTop),
					request);
				if (_invertMedia) {
					if (request.onlyMessageText) {
						SetTextStatePosition(&result, minSymbol, false);
						result.cursor = CursorState::None;
					}
				} else if (request.onlyMessageText) {
					SetTextStatePosition(&result, visibleTextLen, false);
					result.cursor = CursorState::None;
				} else {
					AddTextStateOffset(&result, visibleTextLen);
				}
			} else if (getStateText(point, trect, &result, request)) {
				if (_invertMedia) {
					AddTextStateOffset(&result, visibleMediaTextLen);
				}
				result.overMessageText = true;
				checkBottomInfoState();
				return result;
			} else if (point.y() >= trect.y() + trect.height()) {
				result.symbol = visibleTextLen + visibleMediaTextLen;
			}
		}
		checkBottomInfoState();
		if (const auto size = rightActionSize(); size && _rightAction) {
			const auto fastShareSkip = std::clamp(
				(g.height() - size->height()) / 2,
				0,
				st::historyFastShareBottom);
			const auto fastShareLeft = hasRightLayout()
				? (g.left() - size->width() - st::historyFastShareLeft)
				: (g.left() + g.width() + st::historyFastShareLeft);
			const auto fastShareTop = data()->isSponsored()
				? g.top() + fastShareSkip
				: g.top() + g.height() - fastShareSkip - size->height();
			if (QRect(
				fastShareLeft,
				fastShareTop,
				size->width(),
				size->height()
			).contains(point)) {
				result.link = rightActionLink(point
					- QPoint(fastShareLeft, fastShareTop));
			}
		}
		if (_summarize && _summarize->contains(point)) {
			result.link = _summarize->link();
		}
	} else if (media && media->isDisplayed()) {
		result = media->textState(point - g.topLeft(), request);
		if (request.onlyMessageText) {
			SetTextStatePosition(&result, 0, false);
			result.cursor = CursorState::None;
		}
		AddTextStateOffset(&result, visibleTextLength());
	}

	return result;
}

bool Message::getStateCommentsButton(
		QPoint point,
		QRect &g,
		not_null<TextState*> outResult) const {
	if (!_comments) {
		return false;
	}
	g.setHeight(g.height() - st::historyCommentsButtonHeight);
	if (data()->isSending()
		|| !QRect(
			g.left(),
			g.top() + g.height(),
			g.width(),
			st::historyCommentsButtonHeight).contains(point)) {
		return false;
	}
	if (!_comments->link && data()->repliesAreComments()) {
		_comments->link = createGoToCommentsLink();
	} else if (!_comments->link && data()->externalReply()) {
		_comments->link = prepareRightActionLink();
	}
	outResult->link = _comments->link;
	_comments->lastPoint = point - QPoint(g.left(), g.top() + g.height());
	return true;
}

ClickHandlerPtr Message::createGoToCommentsLink() const {
	const auto fullId = data()->fullId();
	const auto sessionId = data()->history()->session().uniqueId();
	return std::make_shared<LambdaClickHandler>([=](ClickContext context) {
		const auto controller = ExtractController(context);
		if (!controller || controller->session().uniqueId() != sessionId) {
			return;
		}
		if (const auto item = controller->session().data().message(fullId)) {
			const auto history = item->history();
			if (const auto channel = history->peer->asChannel()) {
				if (channel->invitePeekExpires()) {
					controller->showToast(
						tr::lng_channel_invite_private(tr::now));
					return;
				}
			}
			controller->showRepliesForMessage(history, item->id);
		}
	});
}

bool Message::getStateFromName(
		QPoint point,
		QRect &trect,
		not_null<TextState*> outResult) const {
	if (!displayFromName()) {
		return false;
	}
	if (point.y() >= trect.top() && point.y() < trect.top() + st::msgNameFont->height) {
		auto availableLeft = trect.left();
		auto availableWidth = trect.width();
		const auto badgeWidth = rightBadgeWidth();
		if (badgeWidth) {
			availableWidth -= st::msgPadding.right() + badgeWidth;
		}
		const auto item = data();
		const auto from = item->displayFrom();
		const auto nameText = [&]() -> const Ui::Text::String * {
			if (from) {
				validateFromNameText(from);
				return &_fromName;
			} else if (const auto info = item->displayHiddenSenderInfo()) {
				return &info->nameText();
			} else {
				Unexpected("Corrupt forwarded information in message.");
			}
		}();

		const auto statusId = from ? from->emojiStatusId() : EmojiStatusId();
		const auto statusWidth = (from && _fromNameStatus)
			? st::dialogsPremiumIcon.icon.width()
			: 0;
		if (statusWidth && availableWidth > statusWidth) {
			const auto x = availableLeft + std::min(
				availableWidth - statusWidth,
				nameText->maxWidth()
			) - (statusId ? (2 * _fromNameStatus->skip) : 0);
			const auto checkWidth = statusId
				? Ui::Text::AdjustCustomEmojiSize(st::emojiSize)
				: statusWidth;
			if (point.x() >= x && point.x() < x + checkWidth) {
				ensureFromNameStatusLink(from);
				outResult->link = _fromNameStatus->link;
				return true;
			}
		}
		if (point.x() >= availableLeft
			&& point.x() < availableLeft + availableWidth
			&& point.x() < availableLeft + nameText->maxWidth()) {
			outResult->link = fromLink();
			recordLinkRipplePoint(point, trect.topLeft());
			_fromLinkRipplePointSet = 1;
			return true;
		}

		const auto skipWidth = nameText->maxWidth()
			+ (_fromNameStatus
				? (st::dialogsPremiumIcon.icon.width()
					+ st::msgServiceFont->spacew)
				: 0)
			+ st::msgServiceFont->spacew;
		availableLeft += skipWidth;
		availableWidth -= skipWidth;

		auto via = item->Get<HistoryMessageVia>();
		if (via
			&& !displayForwardedFrom()
			&& point.x() >= availableLeft
			&& point.x() < availableLeft + availableWidth
			&& point.x() < availableLeft + via->width) {
			outResult->link = via->link;
			recordLinkRipplePoint(point, trect.topLeft());
			return true;
		}
		if (const auto guestChat = item->Get<HistoryMessageGuestChat>()) {
			auto guestChatLeft = availableLeft;
			if (via && !displayForwardedFrom()) {
				guestChatLeft += via->width + st::msgServiceFont->spacew;
			}
			if (point.x() >= guestChatLeft
				&& point.x() < availableLeft + availableWidth
				&& point.x() < guestChatLeft + guestChat->width) {
				outResult->link = guestChat->link;
				recordLinkRipplePoint(point, trect.topLeft());
				return true;
			}
		}
		if (badgeWidth) {
			const auto badge = Get<RightBadge>();
			const auto badgeLeft = trect.left()
				+ trect.width()
				- badgeWidth;
			const auto badgeRight = trect.left()
				+ trect.width()
				+ st::msgPadding.right();
			const auto boostTextWidth = (badge && !badge->boosts.isEmpty())
				? badge->boosts.maxWidth()
				: 0;
			const auto boostLeft = boostTextWidth
				? (trect.left() + trect.width() - boostTextWidth)
				: 0;
			if (boostTextWidth
				&& point.x() >= boostLeft
				&& point.x() < badgeRight) {
				if (!badge->boostsLink) {
					const auto fullId = item->fullId();
					badge->boostsLink = std::make_shared<LambdaClickHandler>([
						fullId
					](ClickContext context) {
						if (const auto controller = ExtractController(context)) {
							if (const auto item = controller->session().data().message(fullId)) {
								if (const auto channel = item->history()->peer->asChannel()) {
									controller->resolveBoostState(channel);
								}
							}
						}
					});
				}
				outResult->link = badge->boostsLink;
				return true;
			}
			const auto tagRight = boostTextWidth
				? (boostLeft - st::msgTagBadgeBoostSkip)
				: badgeRight;
			if (point.x() >= badgeLeft && point.x() < tagRight) {
				if (badge->special) {
					return false;
				}
				if (!badge->tagLink) {
					const auto weak = base::make_weak(this);
					badge->tagLink = std::make_shared<LambdaClickHandler>([
						weak
					](ClickContext context) {
						if (const auto controller = ExtractController(context)) {
							if (const auto view = weak.get()) {
								const auto badge = view->Get<RightBadge>();
								if (!badge) {
									return;
								}
								const auto item = view->data();
								const auto peer = item->history()->peer;
								const auto author = item->author();
								controller->uiShow()->show(Box(
									TagInfoBox,
									controller->uiShow(),
									peer,
									author,
									badge->tag.toString(),
									badge->role));
							}
						}
					});
				}
				{
					const auto pill = ComputeBadgePillGeometry(badge);
					const auto &padding = st::msgTagBadgePadding;
					if (badge->role != BadgeRole::User) {
						const auto badgeTop = trect.top()
							+ (st::msgNameFont->height - pill.height) / 2;
						badge->lastPoint = point
							- QPoint(badgeLeft, badgeTop);
					} else {
						const auto pillLeft = badgeLeft
							- (pill.width - pill.textWidth) / 2;
						const auto pillTop = trect.top() - padding.top();
						badge->lastPoint = point
							- QPoint(pillLeft, pillTop);
					}
				}
				outResult->link = badge->tagLink;
				return true;
			}
		}
	}
	trect.setTop(trect.top() + st::msgNameFont->height);
	return false;
}

void Message::ensureFromNameStatusLink(not_null<PeerData*> peer) const {
	Expects(_fromNameStatus != nullptr);

	if (_fromNameStatus->link) {
		return;
	}
	_fromNameStatus->link = std::make_shared<LambdaClickHandler>([=](
			ClickContext context) {
		const auto controller = ExtractController(context);
		if (controller) {
			Settings::ShowEmojiStatusPremium(controller, peer);
		}
	});
}

void Message::finishFromNameStatusPaint(
		std::optional<QRect> paintedRect) const {
	if (!_fromNameStatus) {
		return;
	}
	const auto previous = _fromNameStatus->lastRepaintRect;
	_fromNameStatus->lastRepaintRect = paintedRect;
	if (!previous
		|| previous->isEmpty()
		|| (paintedRect && *previous == *paintedRect)) {
		return;
	}
	const auto changed = paintedRect
		? previous->united(*paintedRect)
		: *previous;
	if (!changed.isEmpty()) {
		repaint(changed);
	}
}

void Message::repaintFromNameStatus() const {
	if (!_fromNameStatus) {
		return;
	}
	const auto painted = _fromNameStatus->lastRepaintRect;
	if (!painted) {
		Ui::LogUnknownGeometryRepaint("sender emoji status");
		repaint();
	} else if (!painted->isEmpty()) {
		repaint(*painted);
	}
}

bool Message::getStateTopicButton(
		QPoint point,
		QRect &trect,
		not_null<TextState*> outResult) const {
	if (!displayedTopicButton()) {
		return false;
	}
	trect.setTop(trect.top() + st::topicButtonSkip);
	const auto size = TopicButtonSize(trect.width(), _topicButton->name);
	const auto rect = QRect(trect.topLeft(), size);
	if (rect.contains(point)) {
		outResult->link = _topicButton->link;
		_topicButton->lastPoint = point - rect.topLeft();
		return true;
	}
	trect.setY(trect.y() + size.height() + st::topicButtonSkip);
	return false;
}

bool Message::getStateForwardedInfo(
		QPoint point,
		QRect &trect,
		not_null<TextState*> outResult,
		StateRequest request) const {
	if (!displayForwardedFrom()) {
		return false;
	}
	const auto item = data();
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	const auto skip1 = forwarded->psaType.isEmpty()
		? 0
		: st::historyPsaIconSkip1;
	const auto skip2 = forwarded->psaType.isEmpty()
		? 0
		: st::historyPsaIconSkip2;
	const auto fits = (forwarded->text.maxWidth() <= (trect.width() - skip1));
	const auto fwdheight = (fits ? 1 : 2) * st::semiboldFont->height;
	if (point.y() >= trect.top() && point.y() < trect.top() + fwdheight) {
		if (skip1) {
			const auto &icon = st::historyPsaIconIn;
			const auto position = fits
				? st::historyPsaIconPosition1
				: st::historyPsaIconPosition2;
			const auto iconRect = QRect(
				trect.x() + trect.width() - position.x() - icon.width(),
				trect.y() + position.y(),
				icon.width(),
				icon.height());
			if (iconRect.contains(point)) {
				if (const auto link = psaTooltipLink()) {
					outResult->link = link;
					return true;
				}
			}
		}
		const auto useWidth = trect.width() - (fits ? skip1 : skip2);
		const auto breakEverywhere = (forwarded->text.countHeight(useWidth) > 2 * st::semiboldFont->height);
		auto textRequest = request.forText();
		if (breakEverywhere) {
			textRequest.flags |= Ui::Text::StateRequest::Flag::BreakEverywhere;
		}
		*outResult = TextState(item, forwarded->text.getState(
			point - trect.topLeft(),
			useWidth,
			textRequest));
		if (outResult->link) {
			recordLinkRipplePoint(point, trect.topLeft());
		}
		SetTextStatePosition(outResult, 0, false);
		if (breakEverywhere) {
			outResult->cursor = CursorState::Forwarded;
		} else {
			outResult->cursor = CursorState::None;
		}
		return true;
	}
	trect.setTop(trect.top() + fwdheight);
	return false;
}

ClickHandlerPtr Message::psaTooltipLink() const {
	const auto state = Get<PsaTooltipState>();
	if (!state || !state->buttonVisible) {
		return nullptr;
	} else if (state->link) {
		return state->link;
	}
	const auto type = state->type;
	const auto handler = [=] {
		const auto custom = type.isEmpty()
			? QString()
			: Lang::GetNonDefaultValue(kPsaTooltipPrefix + type.toUtf8());
		auto text = tr::rich(
			(custom.isEmpty()
				? tr::lng_tooltip_psa_default(tr::now)
				: custom));
		TextUtilities::ParseEntities(text, 0);
		psaTooltipToggled(true);
		delegate()->elementShowTooltip(text, crl::guard(this, [=] {
			psaTooltipToggled(false);
		}));
	};
	state->link = std::make_shared<LambdaClickHandler>(
		crl::guard(this, handler));
	return state->link;
}

void Message::psaTooltipToggled(bool tooltipShown) const {
	const auto visible = !tooltipShown;
	const auto state = Get<PsaTooltipState>();
	if (state->buttonVisible == visible) {
		return;
	}
	state->buttonVisible = visible;
	history()->owner().notifyViewLayoutChange(this);
	state->buttonVisibleAnimation.start(
		[=] { repaint(); },
		visible ? 0. : 1.,
		visible ? 1. : 0.,
		st::fadeWrapDuration);
}

bool Message::getStateReplyInfo(
		QPoint point,
		QRect &trect,
		not_null<TextState*> outResult) const {
	if (const auto reply = Get<Reply>()) {
		const auto margins = reply->margins();
		const auto height = reply->height();
		if (point.y() >= trect.top() && point.y() < trect.top() + height) {
			const auto g = QRect(
				trect.x(),
				trect.y() + margins.top(),
				trect.width(),
				height - margins.top() - margins.bottom());
			if (g.contains(point)) {
				if (const auto link = reply->link()) {
					outResult->link = reply->link();
					reply->saveRipplePoint(point - g.topLeft());
				}
			}
			return true;
		}
		trect.setTop(trect.top() + height);
	}
	return false;
}

bool Message::getStateSummaryHeaderInfo(
		QPoint point,
		QRect &trect,
		not_null<TextState*> outResult) const {
	if (const auto summaryHeader = Get<SummaryHeader>()) {
		const auto margins = summaryHeader->margins();
		const auto height = summaryHeader->height();
		if (point.y() >= trect.top() && point.y() < trect.top() + height) {
			const auto g = QRect(
				trect.x(),
				trect.y() + margins.top(),
				trect.width(),
				height - margins.top() - margins.bottom());
			if (g.contains(point)) {
				if (const auto link = summaryHeader->link()) {
					outResult->link = summaryHeader->link();
					summaryHeader->saveRipplePoint(point - g.topLeft());
				}
			}
			return true;
		}
		trect.setTop(trect.top() + height);
	}
	return false;
}

bool Message::getStateViaBotIdInfo(
		QPoint point,
		QRect &trect,
		not_null<TextState*> outResult) const {
	const auto item = data();
	if (const auto via = item->Get<HistoryMessageVia>()) {
		if (!displayFromName() && !displayForwardedFrom()) {
			if (QRect(trect.x(), trect.y(), via->width, st::msgNameFont->height).contains(point)) {
				outResult->link = via->link;
				recordLinkRipplePoint(point, trect.topLeft());
				return true;
			}
			trect.setTop(trect.top() + st::msgNameFont->height);
		}
	}
	return false;
}

bool Message::getStateText(
		QPoint point,
		QRect &trect,
		not_null<TextState*> outResult,
		StateRequest request) const {
	if (!hasVisibleText()) {
		return false;
	}
	const auto item = this->textItem();
	if (const auto rich = richpage()) {
		const auto local = prepareRichPageStateRect(point, trect);
		if (!base::in_range(point.y(), trect.y(), trect.y() + trect.height())) {
			return false;
		}
		const auto clearHorizontalScrollHandler = [&] {
			rich->handlerHorizontalScrollHit = std::nullopt;
			rich->handlerHorizontalScrollPoint = {};
		};
		const auto horizontalScrollHit = rich->article.horizontalScrollHit(local);
		*outResult = TextState(item);
		outResult->horizontalScroll = horizontalScrollHit.scrollable;
		const auto hit = rich->article.hitTest(
			local,
			request.flags | Ui::Text::StateRequest::Flag::LookupSymbol);
		if (horizontalScrollHit.overScrollbar) {
			rich->handlerCodeHeaderSegmentIndex = -1;
			rich->handlerPreparedLink = std::nullopt;
			rich->handlerMediaActivation = {};
			rich->handlerPlaceholderId = {};
			rich->handlerPlaceholderPoint = {};
			if (!rich->handlerHorizontalScrollHit || !rich->handler) {
				rich->handler = std::make_shared<RichPageActionClickHandler>(
					[](ClickContext) {
					});
			}
			rich->handlerHorizontalScrollHit = horizontalScrollHit;
			rich->handlerHorizontalScrollPoint = local;
			outResult->link = rich->handler;
			return true;
		}
		if (!hit.valid()) {
			rich->handlerCodeHeaderSegmentIndex = -1;
			clearHorizontalScrollHandler();
			return horizontalScrollHit.scrollable;
		}
		const auto offset = rich->article.selectionOffsetFromHit(
			hit,
			TextSelectType::Letters);
		SetRichPageSelectionCursor(
			outResult,
			hit.segmentIndex,
			offset,
			hit.direct);
		if (hit.codeHeaderCopy) {
			const auto reuse = rich->handler
				&& (rich->handlerCodeHeaderSegmentIndex == hit.segmentIndex);
			clearHorizontalScrollHandler();
			rich->handlerPreparedLink = std::nullopt;
			rich->handlerMediaActivation = {};
			rich->handlerPlaceholderId = {};
			rich->handlerPlaceholderPoint = {};
			if (!reuse) {
				const auto text = rich->article.textForContext(hit);
				rich->handlerCodeHeaderSegmentIndex = hit.segmentIndex;
				rich->handler = std::make_shared<RichPageActionClickHandler>(
					[text](ClickContext context) {
						CopyRichPageCodeBlockText(text, std::move(context));
					});
			}
			outResult->link = rich->handler;
		} else if (hit.preparedLink
			&& (hit.preparedLink->kind == PreparedLinkKind::External)
			&& (hit.mediaActivation.kind == MediaActivationKind::None)
			&& Iv::Markdown::ExtractPreparedLink(hit.state.link)) {
			rich->handlerCodeHeaderSegmentIndex = -1;
			clearHorizontalScrollHandler();
			rich->handlerPreparedLink = std::nullopt;
			rich->handlerMediaActivation = {};
			rich->handlerPlaceholderId = {};
			rich->handlerPlaceholderPoint = {};
			outResult->link = hit.state.link;
		} else if (hit.preparedLink
			|| hit.mediaActivation.kind != MediaActivationKind::None) {
			const auto prepared = hit.preparedLink;
			const auto activation = hit.mediaActivation;
			const auto reuse = SamePreparedLink(
					rich->handlerPreparedLink,
					prepared)
				&& SameMediaActivation(
					rich->handlerMediaActivation,
					activation);
			rich->handlerCodeHeaderSegmentIndex = -1;
			clearHorizontalScrollHandler();
			rich->handlerPlaceholderId = hit.mediaActivation.placeholderId;
			rich->handlerPlaceholderPoint = hit.placeholderLocalPoint;
			if (!reuse) {
				rich->handlerPreparedLink = prepared;
				rich->handlerMediaActivation = activation;
				rich->handler = std::make_shared<RichPageActionClickHandler>(
					[weak = base::make_weak(const_cast<Message*>(this)),
						prepared,
						activation](ClickContext context) {
						if (const auto owner = weak.get()) {
							if (prepared) {
								owner->activateRichPagePreparedLink(
									*prepared,
									std::move(context));
							} else {
								owner->activateRichPageMedia(
									activation,
									std::move(context));
							}
						}
					},
					prepared);
			}
			outResult->link = rich->handler;
		} else {
			rich->handlerCodeHeaderSegmentIndex = -1;
			clearHorizontalScrollHandler();
			outResult->link = hit.state.link;
		}
		outResult->cursor = (!outResult->link && hit.direct)
			? CursorState::Text
			: CursorState::None;
		return true;
	}
	if (const auto botTop = Get<FakeBotAboutTop>()) {
		trect.setY(trect.y() + botTop->height);
	}
	if (base::in_range(point.y(), trect.y(), trect.y() + trect.height())) {
		*outResult = TextState(item, text().getState(
			point - trect.topLeft(),
			std::max(textRealWidth(), trect.width()),
			request.forText()));
		if (outResult->link
			&& IsRippleLink(outResult->link)
			&& !text().linkRangeFor(outResult->link).empty()) {
			recordLinkRipplePoint(point, trect.topLeft());
		}
		return true;
	}
	return false;
}

// Forward to media.
void Message::updatePressed(QPoint point) {
	if (const auto rich = richpage()
		; rich
		&& rich->handlerHorizontalScrollActive
		&& (ClickHandler::getPressed()
			== rich->handlerHorizontalScrollPressed)) {
		auto trect = QRect();
		if (prepareRichPageTextRect(trect)) {
			(void)rich->article.updateHorizontalScroll(
				prepareRichPageStateRect(point, trect));
		}
	}
	const auto item = data();
	const auto media = this->media();
	if (!media) {
		return;
	}

	auto g = countGeometry();

	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	if (_reactions && !reactionsInBubble) {
		const auto reactionsHeight = st::mediaInBubbleSkip + _reactions->height();
		g.setHeight(g.height() - reactionsHeight);
	}

	const auto keyboard = item->inlineReplyKeyboard();
	if (keyboard) {
		auto keyboardHeight = st::msgBotKbButton.margin + keyboard->naturalHeight();
		g.setHeight(g.height() - keyboardHeight);
	}

	if (drawBubble()) {
		auto mediaDisplayed = media && media->isDisplayed();
		auto trect = g.marginsAdded(-st::msgPadding);
		if (mediaDisplayed && media->isBubbleTop()) {
			trect.setY(trect.y() - st::msgPadding.top());
		} else {
			if (displayFromName()) {
				trect.setTop(trect.top() + st::msgNameFont->height);
			}
			if (displayedTopicButton()) {
				trect.setTop(trect.top()
					+ st::topicButtonSkip
					+ st::topicButtonPadding.top()
					+ st::msgNameFont->height
					+ st::topicButtonPadding.bottom()
					+ st::topicButtonSkip);
			}
			if (displayForwardedFrom()) {
				auto forwarded = item->Get<HistoryMessageForwarded>();
				auto fwdheight = ((forwarded->text.maxWidth() > trect.width()) ? 2 : 1) * st::semiboldFont->height;
				trect.setTop(trect.top() + fwdheight);
			}
			if (const auto reply = Get<Reply>()) {
				trect.setTop(trect.top() + reply->height());
			}
			if (const auto summaryHeader = Get<SummaryHeader>()) {
				trect.setTop(trect.top() + summaryHeader->height());
			}
			if (item->Has<HistoryMessageVia>()) {
				if (!displayFromName() && !displayForwardedFrom()) {
					trect.setTop(trect.top() + st::msgNameFont->height);
				}
			}
		}
		if (mediaDisplayed && media->isBubbleBottom()) {
			trect.setHeight(trect.height() + st::msgPadding.bottom());
		}

		if (mediaDisplayed) {
			auto mediaHeight = media->height();
			auto mediaLeft = trect.x() - st::msgPadding.left();
			auto mediaTop = (trect.y() + trect.height() - mediaHeight);
			media->updatePressed(point - QPoint(mediaLeft, mediaTop));
		}
	} else {
		media->updatePressed(point - g.topLeft());
	}
}

bool Message::consumeHorizontalScroll(QPoint position, int delta) {
	const auto rich = richpage();
	auto trect = QRect();
	if (!rich || !prepareRichPageTextRect(trect)) {
		return false;
	}
	return rich->article.consumeHorizontalScroll(
		prepareRichPageStateRect(position, trect),
		delta);
}

bool Message::canConsumeHorizontalScroll(QPoint position, int delta) const {
	const auto rich = richpage();
	auto trect = QRect();
	if (!rich || !prepareRichPageTextRect(trect)) {
		return false;
	}
	return rich->article.canConsumeHorizontalScroll(
		prepareRichPageStateRect(position, trect),
		delta);
}

MessageSelection Message::selectionFromStates(
		const TextState &anchor,
		const TextState &current,
		TextSelectType type) const {
	if (anchor.selectionCursor.isRichPage()
		|| current.selectionCursor.isRichPage()) {
		if (!anchor.selectionCursor.valid()
			|| !current.selectionCursor.valid()
			|| !anchor.selectionCursor.isRichPage()
			|| !current.selectionCursor.isRichPage()) {
			return {};
		}
		auto selection = MarkdownArticleSelection{
			.from = anchor.selectionCursor.richPagePosition,
			.to = current.selectionCursor.richPagePosition,
		};
		const auto endpoints = MarkdownArticleSelectionEndpoints{
			.from = anchor.selectionCursor.richPage,
			.to = current.selectionCursor.richPage,
		};
		if (type != TextSelectType::Letters) {
			const auto rich = richpage();
			if (!rich) {
				return {};
			}
			selection = AdjustRichPageSelection(
				rich->article,
				selection.from,
				selection.to,
				type);
			if (selection.empty()) {
				return {};
			}
		}
		return MessageSelection::RichPage(
			selection,
			endpoints,
			anchor.selectionCursor.richPagePosition,
			current.selectionCursor.richPagePosition,
			anchor.selectionCursor.richPage,
			current.selectionCursor.richPage);
	}
	const auto anchorEndpoint = FlatSelectionEndpointFromState(anchor);
	const auto currentEndpoint = FlatSelectionEndpointFromState(current);
	auto selection = TextSelection(
		uint16(std::min(anchorEndpoint.offset(), currentEndpoint.offset())),
		uint16(std::max(anchorEndpoint.offset(), currentEndpoint.offset())));
	if (type != TextSelectType::Letters) {
		selection = adjustSelection(selection, type);
	}
	return MessageSelection::Flat(
		selection,
		anchorEndpoint,
		currentEndpoint);
}

TextForMimeData Message::selectedText(TextSelection selection) const {
	const auto media = this->media();
	auto logEntryOriginalResult = TextForMimeData();
	auto factcheckResult = TextForMimeData();
	const auto mediaDisplayed = (media && media->isDisplayed());
	const auto mediaBefore = mediaDisplayed && invertMedia();
	const auto textSelection = mediaBefore
		? media->skipSelection(selection)
		: selection;
	const auto mediaSelection = !invertMedia()
		? skipTextSelection(selection)
		: selection;
	auto textResult = hasVisibleText()
		? text().toTextForMimeData(textSelection)
		: TextForMimeData();
	auto mediaResult = (mediaDisplayed || isHiddenByGroup())
		? media->selectedText(mediaSelection)
		: TextForMimeData();
	if (const auto check = factcheckBlock()) {
		const auto checkSelection = mediaBefore
			? skipTextSelection(textSelection)
			: mediaDisplayed
			? media->skipSelection(mediaSelection)
			: skipTextSelection(selection);
		factcheckResult = check->selectedText(checkSelection);
	}
	if (const auto entry = logEntryOriginal()) {
		const auto originalSelection = mediaBefore
			? skipTextSelection(textSelection)
			: mediaDisplayed
			? media->skipSelection(mediaSelection)
			: skipTextSelection(selection);
		logEntryOriginalResult = entry->selectedText(originalSelection);
	}
	auto &first = mediaBefore ? mediaResult : textResult;
	auto &second = mediaBefore ? textResult : mediaResult;
	auto result = first;
	if (result.empty()) {
		result = std::move(second);
	} else if (!second.empty()) {
		result.append(u"\n\n"_q).append(std::move(second));
	}
	if (result.empty()) {
		result = std::move(factcheckResult);
	} else if (!factcheckResult.empty()) {
		result.append(u"\n\n"_q).append(std::move(factcheckResult));
	}
	if (result.empty()) {
		result = std::move(logEntryOriginalResult);
	} else if (!logEntryOriginalResult.empty()) {
		result.append(u"\n\n"_q).append(std::move(logEntryOriginalResult));
	}
	return result;
}

TextForMimeData Message::selectedText(
		const MessageSelection &selection) const {
	if (const auto flat = selection.flatSelection(); !flat.empty()) {
		return selectedText(flat);
	} else if (selection.isRichPage()) {
		if (const auto rich = richpage()) {
			return rich->article.textForSelection(
				selection.richPage.selection,
				&selection.richPage.endpoints);
		}
	}
	return {};
}

SelectedQuote Message::selectedQuote(TextSelection selection) const {
	const auto textItem = this->textItem();
	const auto item = textItem ? textItem : data().get();
	const auto &translated = item->translatedText();
	const auto &original = item->originalText();
	if (&translated != &original
		|| selection.empty()
		|| selection == FullSelection) {
		return {};
	} else if (hasVisibleText()) {
		const auto media = this->media();
		const auto mediaDisplayed = media && media->isDisplayed();
		const auto mediaBefore = mediaDisplayed && invertMedia();
		const auto textSelection = mediaBefore
			? media->skipSelection(selection)
			: selection;
		return FindSelectedQuote(text(), textSelection, item);
	} else if (const auto media = this->media()) {
		if (media->isDisplayed() || isHiddenByGroup()) {
			return media->selectedQuote(selection);
		}
	}
	return {};
}

SelectedQuote Message::selectedQuote(
		const MessageSelection &selection) const {
	if (const auto flat = selection.flatSelection(); !flat.empty()) {
		return selectedQuote(flat);
	}
	return {};
}

TextSelection Message::selectionFromQuote(
		const SelectedQuote &quote) const {
	Expects(quote.item != nullptr);

	if (quote.highlight.quote.empty()) {
		return {};
	}
	const auto item = quote.item;
	const auto &translated = item->translatedText();
	const auto &original = item->originalText();
	if (&translated != &original) {
		return {};
	} else if (hasVisibleText()) {
		const auto media = this->media();
		const auto mediaDisplayed = media && media->isDisplayed();
		const auto mediaBefore = mediaDisplayed && invertMedia();
		const auto result = FindSelectionFromQuote(text(), quote);
		return mediaBefore ? media->unskipSelection(result) : result;
	} else if (const auto media = this->media()) {
		if (media->isDisplayed() || isHiddenByGroup()) {
			return media->selectionFromQuote(quote);
		}
	}
	return {};
}

TextSelection Message::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	const auto media = this->media();
	const auto mediaDisplayed = media && media->isDisplayed();
	const auto mediaBefore = mediaDisplayed && invertMedia();
	const auto textSelection = mediaBefore
		? media->skipSelection(selection)
		: selection;
	const auto useSelection = [](TextSelection selection, bool skipped) {
		return !skipped || (selection != TextSelection(uint16(), uint16()));
	};
	auto textAdjusted = (hasVisibleText()
		&& useSelection(textSelection, mediaBefore))
		? text().adjustSelection(textSelection, type)
		: textSelection;
	auto textResult = mediaBefore
		? media->unskipSelection(textAdjusted)
		: textAdjusted;
	auto mediaResult = TextSelection();
	auto mediaSelection = mediaBefore
		? selection
		: skipTextSelection(selection);
	if (mediaDisplayed) {
		auto mediaAdjusted = useSelection(mediaSelection, !mediaBefore)
			? media->adjustSelection(mediaSelection, type)
			: mediaSelection;
		mediaResult = mediaBefore
			? mediaAdjusted
			: unskipTextSelection(mediaAdjusted);
	}
	auto checkResult = TextSelection();
	if (const auto check = factcheckBlock()) {
		auto checkSelection = !mediaDisplayed
			? skipTextSelection(selection)
			: mediaBefore
			? skipTextSelection(textSelection)
			: media->skipSelection(mediaSelection);
		auto checkAdjusted = useSelection(checkSelection, true)
			? check->adjustSelection(checkSelection, type)
			: checkSelection;
		checkResult = unskipTextSelection(checkAdjusted);
		if (mediaDisplayed) {
			checkResult = media->unskipSelection(checkResult);
		}
	}
	auto entryResult = TextSelection();
	if (const auto entry = logEntryOriginal()) {
		auto entrySelection = !mediaDisplayed
			? skipTextSelection(selection)
			: mediaBefore
			? skipTextSelection(textSelection)
			: media->skipSelection(mediaSelection);
		auto entryAdjusted = useSelection(entrySelection, true)
			? entry->adjustSelection(entrySelection, type)
			: entrySelection;
		entryResult = unskipTextSelection(entryAdjusted);
		if (mediaDisplayed) {
			entryResult = media->unskipSelection(entryResult);
		}
	}
	auto result = textResult;
	if (!mediaResult.empty()) {
		result = result.empty() ? mediaResult : TextSelection{
			std::min(result.from, mediaResult.from),
			std::max(result.to, mediaResult.to),
		};
	}
	if (!checkResult.empty()) {
		result = result.empty() ? checkResult : TextSelection{
			std::min(result.from, checkResult.from),
			std::max(result.to, checkResult.to),
		};
	}
	if (!entryResult.empty()) {
		result = result.empty() ? entryResult : TextSelection{
			std::min(result.from, entryResult.from),
			std::max(result.to, entryResult.to),
		};
	}
	return result;
}

MessageSelection Message::adjustSelection(
		const MessageSelection &selection,
		TextSelectType type) const {
	if (selection.isFlat()) {
		const auto adjusted = adjustSelection(selection.flatSelection(), type);
		if (adjusted.empty() || (adjusted == FullSelection)) {
			return {};
		}
		return MessageSelection::Flat(
			adjusted,
			selection.anchor.isFlat()
				? selection.anchor.flat
				: MessageSelectionFlatEndpoint{
					.symbol = adjusted.from,
					.afterSymbol = false,
				},
			selection.focus.isFlat()
				? selection.focus.flat
				: MessageSelectionFlatEndpoint{
					.symbol = adjusted.to,
					.afterSymbol = false,
				});
	} else if (selection.isRichPage()) {
		if (type == TextSelectType::Letters
			|| !selection.anchor.valid()
			|| !selection.focus.valid()
			|| !selection.anchor.isRichPage()
			|| !selection.focus.isRichPage()) {
			return selection;
		}
		const auto anchor = selection.anchor.richPagePosition;
		const auto focus = selection.focus.richPagePosition;
		if (!anchor.valid() || !focus.valid()) {
			return selection;
		}
		const auto rich = richpage();
		if (!rich) {
			return {};
		}
		const auto adjusted = AdjustRichPageSelection(
			rich->article,
			anchor,
			focus,
			type);
		if (adjusted.empty()) {
			return {};
		}
		return MessageSelection::RichPage(
			adjusted,
			MarkdownArticleSelectionEndpoints{
				.from = selection.anchor.richPage,
				.to = selection.focus.richPage,
			},
			anchor,
			focus,
			selection.anchor.richPage,
			selection.focus.richPage);
	}
	return {};
}

TextSelection Message::selectionForEdit(
		const MessageSelection &selection) const {
	return selection.isFlat()
		? selection.flatRangeForEdit()
		: TextSelection();
}

bool Message::selectionContains(
		const MessageSelection &selection,
		const TextState &state) const {
	if (!selection.isRichPage()) {
		return Element::selectionContains(selection, state);
	}
	const auto rich = richpage();
	if (!rich
		|| !state.overMessageText
		|| !state.selectionCursor.isRichPage()) {
		return false;
	}
	auto hit = Iv::Markdown::MarkdownArticleHitTestResult();
	hit.segmentIndex = state.selectionCursor.richPagePosition.segment;
	hit.forcedOffset = state.selectionCursor.richPagePosition.offset;
	hit.direct = state.selectionCursor.richPage.direct;
	return rich->article.selectionContains(
		selection.richPage.selection,
		&selection.richPage.endpoints,
		hit);
}

Reactions::ButtonParameters Message::reactionButtonParameters(
		QPoint position,
		const TextState &reactionState) const {
	using namespace Reactions;
	auto result = ButtonParameters{ .context = data()->fullId() };
	const auto outsideBubble = (!_comments && !embedReactionsInBubble());
	const auto geometry = countGeometry();
	result.pointer = position;
	const auto onTheLeft = hasRightLayout();

	const auto keyboard = data()->inlineReplyKeyboard();
	const auto keyboardHeight = keyboard
		? (st::msgBotKbButton.margin + keyboard->naturalHeight())
		: 0;
	const auto reactionsHeight = (_reactions && !embedReactionsInBubble())
		? (st::mediaInBubbleSkip + _reactions->height())
		: 0;
	result.reactionsHeight = reactionsHeight;
	result.keyboardHeight = keyboardHeight;
	const auto innerHeight = geometry.height()
		- keyboardHeight
		- reactionsHeight;
	const auto maybeRelativeCenter = outsideBubble
		? media()->reactionButtonCenterOverride()
		: std::nullopt;
	const auto addOnTheRight = [&] {
		return (maybeRelativeCenter
			|| !(displayFastShare() || displayGoToOriginal()))
			? st::reactionCornerCenter.x()
			: 0;
	};
	const auto relativeCenter = QPoint(
		maybeRelativeCenter.value_or(onTheLeft
			? -st::reactionCornerCenter.x()
			: (geometry.width() + addOnTheRight())),
		innerHeight + st::reactionCornerCenter.y());
	result.center = geometry.topLeft() + relativeCenter;
	if (reactionState.itemId != result.context
		&& !geometry.contains(position)) {
		result.outside = true;
	}
	const auto minSkip = (st::reactionCornerShadow.left()
		+ st::reactionCornerSize.width()
		+ st::reactionCornerShadow.right()) / 2;
	result.center = QPoint(
		std::min(std::max(result.center.x(), minSkip), width() - minSkip),
		result.center.y());
	return result;
}

ReplyButton::ButtonParameters Message::replyButtonParameters(
		QPoint position,
		const TextState &replyState) const {
	using namespace ReplyButton;
	if (!displayFastReply() || unwrapped()) {
		return {};
	}
	auto result = ButtonParameters{ .context = data()->fullId() };
	const auto geometry = countGeometry();
	result.pointer = position;
	const auto reactionInnerRight = st::reactionCornerCenter.x()
		+ st::reactionCornerSize.width() / 2;
	const auto replyInnerWidth = ReplyButton::ComputeInnerWidth();
	const auto relativeCenter = QPoint(
		geometry.width() + reactionInnerRight - replyInnerWidth,
		st::replyCornerCenter.y());
	result.center = geometry.topLeft() + relativeCenter;
	if (replyState.itemId != result.context
		&& !geometry.contains(position)) {
		result.outside = true;
	}
	result.link = fastReplyLink();
	return result;
}

int Message::reactionsOptimalWidth() const {
	return _reactions ? _reactions->countNiceWidth() : 0;
}

void Message::drawInfo(
		Painter &p,
		const PaintContext &context,
		int right,
		int bottom,
		int width,
		InfoDisplayType type) const {
	p.setFont(st::msgDateFont);

	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto stm = context.messageStyle();
	bool invertedsprites = (type == InfoDisplayType::Image)
		|| (type == InfoDisplayType::Background);
	int32 infoRight = right, infoBottom = bottom;
	switch (type) {
	case InfoDisplayType::Default:
		infoRight -= st::msgPadding.right() - st::msgDateDelta.x();
		infoBottom -= st::msgPadding.bottom() - st::msgDateDelta.y();
		p.setPen(stm->msgDateFg);
	break;
	case InfoDisplayType::Image:
		infoRight -= st::msgDateImgDelta + st::msgDateImgPadding.x();
		infoBottom -= st::msgDateImgDelta + st::msgDateImgPadding.y();
		p.setPen(st->msgDateImgFg());
	break;
	case InfoDisplayType::Background:
		infoRight -= st::msgDateImgPadding.x();
		infoBottom -= st::msgDateImgPadding.y();
		p.setPen(st->msgServiceFg());
	break;
	}

	const auto size = _bottomInfo.currentSize();
	const auto dateX = infoRight - size.width();
	const auto dateY = infoBottom - size.height();
	if (type == InfoDisplayType::Image) {
		const auto dateW = size.width() + 2 * st::msgDateImgPadding.x();
		const auto dateH = size.height() + 2 * st::msgDateImgPadding.y();
		Ui::FillRoundRect(p, dateX - st::msgDateImgPadding.x(), dateY - st::msgDateImgPadding.y(), dateW, dateH, sti->msgDateImgBg, sti->msgDateImgBgCorners);
	} else if (type == InfoDisplayType::Background) {
		const auto dateW = size.width() + 2 * st::msgDateImgPadding.x();
		const auto dateH = size.height() + 2 * st::msgDateImgPadding.y();
		Ui::FillRoundRect(p, dateX - st::msgDateImgPadding.x(), dateY - st::msgDateImgPadding.y(), dateW, dateH, sti->msgServiceBg, sti->msgServiceBgCornersSmall);
	}
	_bottomInfo.paint(
		p,
		{ dateX, dateY },
		width,
		delegate()->elementShownUnread(this),
		invertedsprites,
		context);
}

TextState Message::bottomInfoTextState(
		int right,
		int bottom,
		QPoint point,
		InfoDisplayType type) const {
	auto infoRight = right;
	auto infoBottom = bottom;
	switch (type) {
	case InfoDisplayType::Default:
		infoRight -= st::msgPadding.right() - st::msgDateDelta.x();
		infoBottom -= st::msgPadding.bottom() - st::msgDateDelta.y();
		break;
	case InfoDisplayType::Image:
		infoRight -= st::msgDateImgDelta + st::msgDateImgPadding.x();
		infoBottom -= st::msgDateImgDelta + st::msgDateImgPadding.y();
		break;
	case InfoDisplayType::Background:
		infoRight -= st::msgDateImgPadding.x();
		infoBottom -= st::msgDateImgPadding.y();
		break;
	}
	const auto size = _bottomInfo.currentSize();
	const auto infoLeft = infoRight - size.width();
	const auto infoTop = infoBottom - size.height();
	return _bottomInfo.textState(
		this,
		point - QPoint{ infoLeft, infoTop });
}

int Message::infoWidth() const {
	return _bottomInfo.maxWidth();
}

int Message::bottomInfoFirstLineWidth() const {
	return _bottomInfo.firstLineWidth();
}

bool Message::bottomInfoIsWide() const {
	if (_reactions && embedReactionsInBubble()) {
		return false;
	}
	return _bottomInfo.isWide();
}

bool Message::isSignedAuthorElided() const {
	return _bottomInfo.isSignedAuthorElided();
}

bool Message::embedReactionsInBubble() const {
	return needInfoDisplay();
}

void Message::validateFromNameText(PeerData *from) const {
	const auto clearStatus = [&] {
		if (_fromNameStatus) {
			_fromNameStatus = nullptr;
			const_cast<Message*>(this)->checkHeavyPart();
		}
	};
	if (!from) {
		clearStatus();
		return;
	}
	const auto version = from->nameVersion();
	if (_fromNameVersion < version) {
		_fromNameVersion = version;
		_fromName.setText(
			st::msgNameStyle,
			from->name(),
			Ui::NameTextOptions());
	}
	if (from->isPremium()
		|| (from->isChannel()
			&& from->emojiStatusId()
			&& from != history()->peer)) {
		if (!_fromNameStatus) {
			_fromNameStatus = std::make_unique<FromNameStatus>();
			const auto size = st::emojiSize;
			const auto emoji = Ui::Text::AdjustCustomEmojiSize(size);
			_fromNameStatus->skip = (size - emoji) / 2;
		}
	} else {
		clearStatus();
	}
}

bool Message::updateBottomInfo() {
	const auto wasInfo = _bottomInfo.currentSize();
	_bottomInfo.update(BottomInfoDataFromMessage(this), width());
	return (_bottomInfo.currentSize() != wasInfo);
}

void Message::itemDataChanged() {
	const auto infoChanged = updateBottomInfo();
	const auto reactionsChanged = updateReactions();
	const auto media = this->media();
	const auto mediaChanged = media && media->updateItemData();
	if (infoChanged || reactionsChanged || mediaChanged) {
		history()->owner().requestViewResize(this);
	} else {
		repaint();
	}
}

auto Message::verticalRepaintRange() const -> VerticalRepaintRange {
	const auto media = this->media();
	const auto add = media ? media->bubbleRollRepaintMargins() : QMargins();
	return {
		.top = -add.top(),
		.height = height() + add.top() + add.bottom()
	};
}

void Message::refreshDataIdHook() {
	if (_rightAction && base::take(_rightAction->link)) {
		_rightAction->link = rightActionLink(_rightAction->lastPoint);
	}
	if (base::take(_fastReplyLink)) {
		_fastReplyLink = fastReplyLink();
	}
	if (_viewButton) {
		_viewButton = nullptr;
		updateViewButtonExistence();
	}
	if (_comments) {
		_comments->link = nullptr;
	}
}

int Message::monospaceMaxWidth() const {
	const auto fromText = hasRichPage()
		? std::max(
			textualMaxWidth()
				- st::msgPadding.left()
				- st::msgPadding.right(),
			richpage()->article.lastLayoutWidth())
		: hasVisibleText()
		? text().countMaxMonospaceWidth()
		: 0;
	const auto fromMedia = this->media()
		? this->media()->contributedMaxMonospaceWidth()
		: 0;
	return st::msgPadding.left()
		+ std::max(fromText, fromMedia)
		+ st::msgPadding.right();
}

int Message::bubbleTextWidth(int bubbleWidth) const {
	// For rich pages the article is laid out exactly at the bubble width,
	// so richPageWidthFor(bubbleTextWidth(width)) must give width back.
	// Clamping by msgMinWidth here would lay the article out wider than
	// the bubble shrunk to its content, painting centered blocks (like
	// display math formulas) shifted right and cut by the bubble edge.
	const auto floored = hasRichPage()
		? bubbleWidth
		: std::max(bubbleWidth, st::msgMinWidth);
	return floored
		- st::msgPadding.left()
		- st::msgPadding.right();
}

int Message::bubbleTextualWidth() const {
	const auto full = textualMaxWidth();
	if (hasRichPage()) {
		const auto innerWidth = bubbleTextWidth(full);
		[[maybe_unused]] const auto laidOutHeight = textHeightFor(innerWidth);
		const auto laidOutWidth = richpage()->article.lastLayoutWidth();
		return st::msgPadding.left()
			+ std::max(laidOutWidth, 1)
			+ st::msgPadding.right();
	}
	const auto media = this->media();
	if (!hasVisibleText()
		|| !media
		|| !media->allowsNarrowBubble()) {
		return full;
	}
	const auto minimum = std::max(
		media->minBubbleWidthForNarrowBubble(),
		st::msgMinWidth);
	if (_bubbleTextualWidthMinimum != minimum) {
		_bubbleTextualWidthMinimum = minimum;
		if (minimum >= full) {
			_bubbleTextualWidthCache = minimum;
		} else {
			const auto lineHeight = text().style()->font->height;
			const auto fullTextHeight = textHeightFor(bubbleTextWidth(full));
			if (fullTextHeight > kMaxNiceToReadLines * lineHeight) {
				_bubbleTextualWidthCache = full;
			} else {
				auto left = minimum;
				auto right = full;
				while (left < right) {
					const auto middle = left + (right - left) / 2;
					const auto middleHeight = textHeightFor(
						bubbleTextWidth(middle));
					if (middleHeight <= kMaxNiceToReadLines * lineHeight) {
						right = middle;
					} else {
						left = middle + 1;
					}
				}
				_bubbleTextualWidthCache = right;
				[[maybe_unused]] const auto ensureRightCache
					= textHeightFor(bubbleTextWidth(right));
			}
		}
	}
	return _bubbleTextualWidthCache;
}

int Message::viewButtonHeight() const {
	return _viewButton ? _viewButton->height() : 0;
}

void Message::updateViewButtonExistence() {
	const auto item = data();
	const auto make = [=](auto &&from) {
		return std::make_unique<ViewButton>(
			std::forward<decltype(from)>(from),
			colorIndex(),
			[=] { repaint(); });
	};
	if (const auto richPage = item->richPage(); richPage && richPage->part) {
		const auto itemId = item->fullId();
		if (_viewButton && _viewButton->matches(itemId)) {
			return;
		}
		_viewButton = make(itemId);
		return;
	}
	const auto media = item->media();
	if (media && ViewButton::MediaHasViewButton(media)) {
		if (_viewButton && _viewButton->matches(media)) {
			return;
		}
		_viewButton = make(media);
		return;
	}
	_viewButton = nullptr;
}

void Message::initLogEntryOriginal() {
	if (const auto log = data()->Get<HistoryMessageLogEntryOriginal>()) {
		AddComponents(LogEntryOriginal::Bit());
		const auto entry = Get<LogEntryOriginal>();
		using Flags = MediaWebPageFlags;
		entry->page = std::make_unique<WebPage>(this, log->page, Flags());
	}
}

void Message::initPsa() {
	if (const auto forwarded = data()->Get<HistoryMessageForwarded>()) {
		if (!forwarded->psaType.isEmpty()) {
			AddComponents(PsaTooltipState::Bit());
			Get<PsaTooltipState>()->type = forwarded->psaType;
		}
	}
}

WebPage *Message::logEntryOriginal() const {
	if (const auto entry = Get<LogEntryOriginal>()) {
		return entry->page.get();
	}
	return nullptr;
}

WebPage *Message::factcheckBlock() const {
	if (const auto entry = Get<Factcheck>()) {
		return entry->page.get();
	}
	return nullptr;
}

namespace {

template <typename MediaData>
void RegisterHostedMedia(
		base::flat_map<
			const MediaData*,
			std::vector<HostedMediaEntry>> &mediaByData,
		not_null<Media*> media,
		const MediaData *data,
		Fn<bool()> active) {
	auto &list = mediaByData[data];
	for (auto i = begin(list); i != end(list);) {
		const auto existing = i->media.get();
		if (!existing) {
			i = list.erase(i);
		} else if (existing == media.get()) {
			i->active = std::move(active);
			return;
		} else {
			++i;
		}
	}
	list.push_back({
		.media = base::make_weak(media.get()),
		.active = std::move(active),
	});
}

template <typename MediaData, typename Callback>
void DispatchHostedMedia(
		base::flat_map<
			const MediaData*,
			std::vector<HostedMediaEntry>> &mediaByData,
		const MediaData *data,
		Callback callback) {
	const auto found = mediaByData.find(data);
	if (found == end(mediaByData)) {
		return;
	}
	auto &list = found->second;
	for (auto i = begin(list); i != end(list);) {
		const auto media = i->media.get();
		if (!media) {
			i = list.erase(i);
		} else {
			if (!i->active || i->active()) {
				callback(not_null{ media });
			}
			++i;
		}
	}
	if (list.empty()) {
		mediaByData.erase(found);
	}
}

} // namespace

void Message::playbackUpdated(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) const {
	const auto hosted = Get<HostedMediaUpdates>();
	if (!hosted || !hosted->suppressBackingMedia) {
		Element::playbackUpdated(item, document);
		if (const auto page = logEntryOriginal()) {
			static_cast<void>(page->playbackUpdated(item, document));
		}
		if (const auto page = factcheckBlock()) {
			static_cast<void>(page->playbackUpdated(item, document));
		}
	}
	if (hosted) {
		DispatchHostedMedia(
			hosted->mediaByDocument,
			document.get(),
			[&](not_null<Media*> media) {
				static_cast<void>(media->playbackUpdated(item, document));
			});
	}
}

void Message::transferUpdated(
		not_null<const HistoryItem*> item,
		not_null<const PhotoData*> photo) const {
	const auto hosted = Get<HostedMediaUpdates>();
	if (!hosted || !hosted->suppressBackingMedia) {
		Element::transferUpdated(item, photo);
		if (const auto page = logEntryOriginal()) {
			page->transferUpdated(item, photo);
		}
		if (const auto page = factcheckBlock()) {
			page->transferUpdated(item, photo);
		}
	}
	if (hosted) {
		DispatchHostedMedia(
			hosted->mediaByPhoto,
			photo.get(),
			[&](not_null<Media*> media) {
				media->transferUpdated(item, photo);
			});
	}
}

void Message::transferUpdated(
		not_null<const HistoryItem*> item,
		not_null<const DocumentData*> document) const {
	const auto hosted = Get<HostedMediaUpdates>();
	if (!hosted || !hosted->suppressBackingMedia) {
		Element::transferUpdated(item, document);
		if (const auto page = logEntryOriginal()) {
			page->transferUpdated(item, document);
		}
		if (const auto page = factcheckBlock()) {
			page->transferUpdated(item, document);
		}
	}
	if (hosted) {
		DispatchHostedMedia(
			hosted->mediaByDocument,
			document.get(),
			[&](not_null<Media*> media) {
				media->transferUpdated(item, document);
			});
	}
}

void Message::suppressBackingMediaForHostedMedia() {
	AddComponents(HostedMediaUpdates::Bit());
	Get<HostedMediaUpdates>()->suppressBackingMedia = true;
}

void Message::registerHostedMedia(not_null<Media*> media) {
	if (const auto photo = media->getPhoto()) {
		registerHostedMedia(media, not_null{ photo });
	}
	if (const auto document = media->getDocument()) {
		registerHostedMedia(media, not_null{ document });
	}
}

void Message::registerHostedMedia(
		not_null<Media*> media,
		not_null<const PhotoData*> photo,
		Fn<bool()> active) {
	const auto hosted = Get<HostedMediaUpdates>();
	Expects(hosted != nullptr);
	RegisterHostedMedia(
		hosted->mediaByPhoto,
		media,
		photo.get(),
		std::move(active));
}

void Message::registerHostedMedia(
		not_null<Media*> media,
		not_null<const DocumentData*> document,
		Fn<bool()> active) {
	const auto hosted = Get<HostedMediaUpdates>();
	Expects(hosted != nullptr);
	RegisterHostedMedia(
		hosted->mediaByDocument,
		media,
		document.get(),
		std::move(active));
}

bool Message::toggleSelectionByHandlerClick(
		const ClickHandlerPtr &handler) const {
	if (_comments && _comments->link == handler) {
		return true;
	} else if (_viewButton && _viewButton->link() == handler) {
		return true;
	} else if (const auto media = this->media()) {
		if (media->toggleSelectionByHandlerClick(handler)) {
			return true;
		}
	}
	return false;
}

bool Message::allowTextSelectionByHandler(
		const ClickHandlerPtr &handler) const {
	if (const auto media = this->media()) {
		if (media->allowTextSelectionByHandler(handler)) {
			return true;
		}
	}
	if (dynamic_cast<Ui::Text::BlockquoteClickHandler*>(handler.get())) {
		return true;
	}
	return false;
}

bool Message::hasFromName() const {
	switch (context()) {
	case Context::AdminLog:
		return true;
	case Context::Monoforum:
		return data()->out() || data()->from()->isChannel();
	case Context::History:
	case Context::ChatPreview:
	case Context::TTLViewer:
	case Context::Pinned:
	case Context::Replies:
	case Context::SavedSublist:
	case Context::ScheduledTopic: {
		const auto item = data();
		const auto peer = item->history()->peer;
		if (hasOutLayout() && !item->from()->isChannel()) {
			if (peer->isSelf()) {
				if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
					return forwarded->savedFromSender
						&& forwarded->savedFromSender->isChannel();
				}
			}
			return false;
		} else if (!peer->isUser() || item->isGuestChatBotMessage()) {
			if (const auto media = this->media()) {
				return !media->hideFromName();
			}
			return true;
		}
		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			if (forwarded->imported
				&& peer.get() == forwarded->originalSender) {
				return false;
			} else if (item->showForwardsFromSender(forwarded)) {
				return true;
			}
		}
		return false;
	} break;
	case Context::ContactPreview:
	case Context::ShortcutMessages:
		return false;
	}
	Unexpected("Context in Message::hasFromName.");
}

bool Message::displayFromName() const {
	if (!hasFromName() || isAttachedToPrevious() || data()->isSponsored()) {
		return false;
	}
	return !Has<PsaTooltipState>();
}

bool Message::displayForwardedFrom() const {
	const auto item = data();
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (forwarded->story) {
			return true;
		} else if (item->showForwardsFromSender(forwarded)) {
			return forwarded->savedFromHiddenSenderInfo
				|| (forwarded->savedFromSender
					&& (forwarded->savedFromSender
						!= forwarded->originalSender));
		}
		if (const auto sender = item->discussionPostOriginalSender()) {
			if (sender == forwarded->originalSender) {
				return false;
			}
		}
		const auto media = item->media();
		return !media || !media->dropForwardedInfo();
	}
	return false;
}

bool Message::hasOutLayout() const {
	const auto item = data();
	if (item->history()->peer->isSelf()) {
		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			if (context() == Context::ShortcutMessages) {
				return true;
			}
			return (context() == Context::SavedSublist
					|| context() == Context::History)
				&& (!forwarded->forwardOfForward()
					? (forwarded->originalSender
						&& forwarded->originalSender->isSelf())
					: ((forwarded->savedFromSender
						&& forwarded->savedFromSender->isSelf())
						|| forwarded->savedFromOutgoing));
		}
		return true;
	} else if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (!forwarded->imported
			|| !forwarded->originalSender
			|| !forwarded->originalSender->isSelf()) {
			if (item->showForwardsFromSender(forwarded)) {
				return false;
			}
		}
	}
	if (item->isGuestChatBotMessage()) {
		return false;
	}
	return item->out() && !item->isPost();
}

bool Message::drawBubble() const {
	const auto item = data();
	if (isHidden()) {
		return false;
	} else if (logEntryOriginal()
		|| factcheckBlock()
		|| item->isFakeAboutView()) {
		return true;
	}
	const auto media = this->media();
	return media
		? (hasVisibleText() || media->needsBubble())
		: !item->isEmpty();
}

bool Message::hasBubble() const {
	return drawBubble();
}

TopicButton *Message::displayedTopicButton() const {
	return _topicButton.get();
}

bool Message::unwrapped() const {
	const auto item = data();
	if (isHidden()) {
		return true;
	} else if (logEntryOriginal() || factcheckBlock()) {
		return false;
	}
	const auto media = this->media();
	return media
		? (!hasVisibleText() && media->unwrapped())
		: item->isEmpty();
}

int Message::minWidthForMedia() const {
	// InstantViewMediaRuntime without a rich page means a media view
	// hosted inside an article, it doesn't draw a bubble with info.
	// Rich page messages are real bubbles and need the minimal width.
	if (Get<InstantViewMediaRuntime>() && !hasRichPage()) {
		return 0;
	}
	auto result = infoWidth() + 2 * (st::msgDateImgDelta + st::msgDateImgPadding.x());
	const auto views = data()->Get<HistoryMessageViews>();
	if (data()->repliesAreComments() && !views->replies.text.isEmpty()) {
		const auto limit = HistoryMessageViews::kMaxRecentRepliers;
		const auto single = st::historyCommentsUserpics.size;
		const auto shift = st::historyCommentsUserpics.shift;
		const auto added = single
			+ (limit - 1) * (single - shift)
			+ st::historyCommentsSkipLeft
			+ st::historyCommentsSkipRight
			+ st::historyCommentsSkipText
			+ st::historyCommentsOpenOutSelected.width()
			+ st::historyCommentsSkipRight
			+ st::mediaUnreadSkip
			+ st::mediaUnreadSize;
		accumulate_max(result, added + views->replies.textWidth);
	} else if (data()->externalReply()) {
		const auto added = st::historyCommentsIn.width()
			+ st::historyCommentsSkipLeft
			+ st::historyCommentsSkipRight
			+ st::historyCommentsSkipText
			+ st::historyCommentsOpenOutSelected.width()
			+ st::historyCommentsSkipRight;
		accumulate_max(result, added + st::semiboldFont->width(
			tr::lng_replies_view_original(tr::now)));
	}
	if (const auto keyboard = data()->inlineReplyKeyboard()) {
		accumulate_max(result, keyboard->naturalWidth());
	}
	return result;
}

bool Message::hasFastReply() const {
	if (context() == Context::Replies) {
		if (isCommentsRootView()) {
			return false;
		}
	} else if (context() != Context::History) {
		return false;
	}
	const auto peer = data()->history()->peer;
	return !hasOutLayout() && (peer->isChat() || peer->isMegagroup());
}

bool Message::displayFastReply() const {
	const auto canSendAnything = [&] {
		const auto item = data();
		const auto peer = item->history()->peer;
		const auto topic = item->topic();
		return topic
			? Data::CanSendAnything(topic)
			: Data::CanSendAnything(peer);
	};

	return hasFastReply()
		&& data()->isRegular()
		&& canSendAnything()
		&& !delegate()->elementInSelectionMode(this).inSelectionMode;
}

bool Message::displayRightActionComments() const {
	return !isPinnedContext()
		&& (context() != Context::SavedSublist)
		&& data()->repliesAreComments()
		&& media()
		&& media()->isDisplayed()
		&& !hasBubble();
}

std::optional<QSize> Message::rightActionSize() const {
	if (displayRightActionComments()) {
		const auto views = data()->Get<HistoryMessageViews>();
		Assert(views != nullptr);
		return (views->repliesSmall.textWidth > 0)
			? QSize(
				std::max(
					st::historyFastShareSize,
					2 * st::historyFastShareBottom + views->repliesSmall.textWidth),
				st::historyFastShareSize + st::historyFastShareBottom + st::semiboldFont->height)
			: QSize(st::historyFastShareSize, st::historyFastShareSize);
	}
	return data()->isSponsored()
		? ((_rightAction && _rightAction->second)
			? QSize(st::historyFastCloseSize, st::historyFastCloseSize * 2)
			: QSize(st::historyFastCloseSize, st::historyFastCloseSize))
		: (displayFastShare() || displayGoToOriginal())
		? QSize(st::historyFastShareSize, st::historyFastShareSize)
		: std::optional<QSize>();
}

bool Message::displayFastShare() const {
	const auto item = data();
	const auto peer = item->history()->peer;
	if (!item->allowsForward()) {
		return false;
	} else if (peer->isChannel()) {
		return !peer->isMegagroup();
	} else if (const auto user = peer->asUser()) {
		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			return !item->out()
				&& forwarded->originalSender
				&& forwarded->originalSender->isBroadcast()
				&& !item->showForwardsFromSender(forwarded);
		} else if (user->isBot() && !item->out()) {
			if (const auto media = this->media()) {
				return media->allowsFastShare();
			}
		}
	}
	return false;
}

bool Message::displayGoToOriginal() const {
	if (isPinnedContext()) {
		return !hasOutLayout();
	} else if (context() == Context::Monoforum) {
		return false;
	}
	const auto item = data();
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		return forwarded->savedFromPeer
			&& forwarded->savedFromMsgId
			&& (!item->externalReply() || !hasBubble())
			&& (context() != Context::Replies);
	}
	return false;
}

void Message::drawRightAction(
		Painter &p,
		const PaintContext &context,
		int left,
		int top,
		int outerWidth) const {
	ensureRightAction();

	const auto size = rightActionSize();
	const auto st = context.st;

	if (_rightAction->ripple) {
		const auto &stm = context.messageStyle();
		const auto colorOverride = &stm->msgWaveformInactive->c;
		_rightAction->ripple->paint(
			p,
			left,
			top,
			size->width(),
			colorOverride);
		if (_rightAction->ripple->empty()) {
			_rightAction->ripple.reset();
		}
	}
	if (_rightAction->second && _rightAction->second->ripple) {
		const auto &stm = context.messageStyle();
		const auto colorOverride = &stm->msgWaveformInactive->c;
		_rightAction->second->ripple->paint(
			p,
			left,
			top + st::historyFastCloseSize,
			size->width(),
			colorOverride);
		if (_rightAction->second->ripple->empty()) {
			_rightAction->second->ripple.reset();
		}
	}

	p.setPen(Qt::NoPen);
	p.setBrush(st->msgServiceBg());
	{
		PainterHighQualityEnabler hq(p);
		const auto rect = style::rtlrect(
			left,
			top,
			size->width(),
			size->height(),
			outerWidth);
		const auto usual = st::historyFastShareSize;
		if (size->width() == size->height() && size->width() == usual) {
			p.drawEllipse(rect);
		} else {
			p.drawRoundedRect(rect, usual / 2, usual / 2);
		}
	}
	if (displayRightActionComments()) {
		const auto &icon = st->historyFastCommentsIcon();
		icon.paint(
			p,
			left + (size->width() - icon.width()) / 2,
			top + (st::historyFastShareSize - icon.height()) / 2,
			outerWidth);
		const auto views = data()->Get<HistoryMessageViews>();
		Assert(views != nullptr);
		if (views->repliesSmall.textWidth > 0) {
			p.setPen(st->msgServiceFg());
			p.setFont(st::semiboldFont);
			p.drawTextLeft(
				left + (size->width() - views->repliesSmall.textWidth) / 2,
				top + st::historyFastShareSize,
				outerWidth,
				views->repliesSmall.text,
				views->repliesSmall.textWidth);
		}
	} else if (_rightAction->second) {
		st->historyFastCloseIcon().paintInCenter(
			p,
			QRect(left, top, size->width(), size->width()));
		st->historyFastMoreIcon().paintInCenter(
			p,
			QRect(left, size->width() + top, size->width(), size->width()));
	} else {
		const auto &icon = data()->isSponsored()
			? st->historyFastCloseIcon()
			: (displayFastShare()
				&& !isPinnedContext()
				&& this->context() != Context::SavedSublist)
			? st->historyFastShareIcon()
			: st->historyGoToOriginalIcon();
		icon.paintInCenter(p, Rect(left, top, *size));
	}
}

ClickHandlerPtr Message::rightActionLink(
		std::optional<QPoint> pressPoint) const {
	if (delegate()->elementInSelectionMode(this).progress > 0) {
		return nullptr;
	}
	ensureRightAction();
	if (!_rightAction->link) {
		_rightAction->link = prepareRightActionLink();
	}
	if (pressPoint) {
		_rightAction->lastPoint = *pressPoint;
	}
	if (_rightAction->second
		&& (_rightAction->lastPoint.y() > st::historyFastCloseSize)) {
		return _rightAction->second->link;
	}
	return _rightAction->link;
}

void Message::ensureRightAction() const {
	if (_rightAction) {
		return;
	}
	Assert(rightActionSize().has_value());
	_rightAction = std::make_unique<RightAction>();
}

ClickHandlerPtr Message::prepareRightActionLink() const {
	if (data()->isSponsored()) {
		return HideSponsoredClickHandler();
	} else if (isPinnedContext()) {
		return JumpToMessageClickHandler(data());
	} else if ((context() != Context::SavedSublist)
		&& displayRightActionComments()) {
		return createGoToCommentsLink();
	}
	const auto sessionId = data()->history()->session().uniqueId();
	const auto owner = &data()->history()->owner();
	const auto itemId = data()->fullId();
	const auto forwarded = data()->Get<HistoryMessageForwarded>();
	const auto savedFromPeer = forwarded
		? forwarded->savedFromPeer
		: nullptr;
	const auto savedFromMsgId = forwarded ? forwarded->savedFromMsgId : 0;

	using Callback = FnMut<void(not_null<Window::SessionController*>)>;
	const auto showByThread = std::make_shared<Callback>();
	const auto showByThreadWeak = std::weak_ptr<Callback>(showByThread);
	if (data()->externalReply()) {
		*showByThread = [=, requested = 0](
				not_null<Window::SessionController*> controller) mutable {
			const auto original = savedFromPeer->owner().message(
				savedFromPeer,
				savedFromMsgId);
			if (original && original->replyToTop()) {
				controller->showRepliesForMessage(
					original->history(),
					original->replyToTop(),
					original->id,
					Window::SectionShow::Way::Forward);
			} else if (!requested) {
				const auto prequested = &requested;
				requested = 1;
				savedFromPeer->session().api().requestMessageData(
					savedFromPeer,
					savedFromMsgId,
					[=, weak = base::make_weak(controller)] {
						if (const auto strong = showByThreadWeak.lock()) {
							if (const auto strongController = weak.get()) {
								*prequested = 2;
								(*strong)(strongController);
							}
						}
					});
			} else if (requested == 2) {
				controller->showPeerHistory(
					savedFromPeer,
					Window::SectionShow::Way::Forward,
					savedFromMsgId);
			}
		};
	};

	class FastShareClickHandler : public LambdaClickHandler {
	public:
		FastShareClickHandler(Fn<void(ClickContext)> handler)
			: LambdaClickHandler(std::move(handler)) {}
		QString tooltip() const override {
			return tr::lng_fast_share_tooltip(tr::now);
		}
	};

	const auto result = std::make_shared<FastShareClickHandler>([=](
			ClickContext context) {
		const auto controller = ExtractController(context);
		if (!controller || controller->session().uniqueId() != sessionId) {
			return;
		}

		if (const auto item = owner->message(itemId)) {
			if (*showByThread) {
				(*showByThread)(controller);
			} else if (savedFromPeer && savedFromMsgId) {
				controller->showPeerHistory(
					savedFromPeer,
					Window::SectionShow::Way::Forward,
					savedFromMsgId);
			} else if (base::IsCtrlPressed()) {
				FastShareMessageToSelf(controller->uiShow(), item);
			} else {
				FastShareMessage(controller, item);
			}
		}
	});
	const auto navigates = data()->externalReply()
		|| (savedFromPeer && savedFromMsgId);
	if (!navigates) {
		result->setProperty(kFastShareProperty, QVariant::fromValue(true));
	}
	return result;
}

ClickHandlerPtr Message::fastReplyLink() const {
	if (_fastReplyLink) {
		return _fastReplyLink;
	}
	const auto itemId = data()->fullId();
	_fastReplyLink = std::make_shared<LambdaClickHandler>(crl::guard(this, [=] {
		delegate()->elementReplyTo({ itemId });
	}));
	return _fastReplyLink;
}

bool Message::isPinnedContext() const {
	return context() == Context::Pinned;
}

void Message::updateMediaInBubbleState() {
	const auto item = data();
	const auto media = this->media();

	if (media) {
		media->updateNeedBubbleState();
	}
	const auto reactionsInBubble = (_reactions && embedReactionsInBubble());
	auto mediaHasSomethingBelow = (_viewButton != nullptr)
		|| reactionsInBubble
		|| (invertMedia() && hasVisibleText());
	auto mediaHasSomethingAbove = false;
	auto getMediaHasSomethingAbove = [&] {
		return displayFromName()
			|| displayedTopicButton()
			|| displayForwardedFrom()
			|| Has<Reply>()
			|| Has<EphemeralBadge>()
			|| item->Has<HistoryMessageVia>();
	};
	const auto entry = logEntryOriginal();
	const auto check = factcheckBlock();
	if (check) {
		mediaHasSomethingBelow = true;
		mediaHasSomethingAbove = getMediaHasSomethingAbove();
		auto checkState = (mediaHasSomethingAbove
			|| hasVisibleText()
			|| (media && media->isDisplayed()))
			? MediaInBubbleState::Bottom
			: MediaInBubbleState::None;
		check->setInBubbleState(checkState);
		if (!media) {
			check->setBubbleRounding(countBubbleRounding());
			return;
		}
	} else if (entry) {
		mediaHasSomethingBelow = true;
		mediaHasSomethingAbove = getMediaHasSomethingAbove();
		auto entryState = (mediaHasSomethingAbove
			|| hasVisibleText()
			|| (media && media->isDisplayed()))
			? MediaInBubbleState::Bottom
			: MediaInBubbleState::None;
		entry->setInBubbleState(entryState);
		if (!media) {
			entry->setBubbleRounding(countBubbleRounding());
			return;
		}
	} else if (!media) {
		return;
	}

	const auto guard = gsl::finally([&] {
		media->setBubbleRounding(countBubbleRounding());
	});
	if (!drawBubble()) {
		media->setInBubbleState(MediaInBubbleState::None);
		return;
	}

	if (!check && !entry) {
		mediaHasSomethingAbove = getMediaHasSomethingAbove();
	}
	if (!invertMedia() && hasVisibleText()) {
		mediaHasSomethingAbove = true;
	}
	const auto state = [&] {
		if (mediaHasSomethingAbove) {
			if (mediaHasSomethingBelow) {
				return MediaInBubbleState::Middle;
			}
			return MediaInBubbleState::Bottom;
		} else if (mediaHasSomethingBelow) {
			return MediaInBubbleState::Top;
		}
		return MediaInBubbleState::None;
	}();
	media->setInBubbleState(state);
}

void Message::fromNameUpdated(int width) const {
	const auto item = data();
	if (Has<RightBadge>()) {
		width -= st::msgPadding.right() + rightBadgeWidth();
	}
	const auto from = item->displayFrom();
	validateFromNameText(from);
	if (const auto via = item->Get<HistoryMessageVia>()) {
		if (!displayForwardedFrom()) {
			const auto nameText = [&]() -> const Ui::Text::String * {
				if (from) {
					return &_fromName;
				} else if (const auto info = item->originalHiddenSenderInfo()) {
					return &info->nameText();
				} else {
					Unexpected("Corrupted forwarded information in message.");
				}
			}();
			via->resize(width
				- st::msgPadding.left()
				- st::msgPadding.right()
				- nameText->maxWidth()
				- (_fromNameStatus
					? (st::dialogsPremiumIcon.icon.width()
						+ st::msgServiceFont->spacew)
					: 0)
				- st::msgServiceFont->spacew);
		}
	}
	if (const auto guestChat = item->Get<HistoryMessageGuestChat>()) {
		const auto nameText = [&]() -> const Ui::Text::String * {
			if (from) {
				return &_fromName;
			} else if (const auto info = item->originalHiddenSenderInfo()) {
				return &info->nameText();
			} else {
				Unexpected("Corrupted forwarded information in message.");
			}
		}();
		auto viaWidth = 0;
		if (const auto via = item->Get<HistoryMessageVia>()) {
			if (!displayForwardedFrom()) {
				viaWidth = st::msgServiceFont->spacew + via->width;
			}
		}
		guestChat->resize(width
			- st::msgPadding.left()
			- st::msgPadding.right()
			- nameText->maxWidth()
			- (_fromNameStatus
				? (st::dialogsPremiumIcon.icon.width()
					+ st::msgServiceFont->spacew)
				: 0)
			- st::msgServiceFont->spacew
			- viaWidth);
	}
}

TextSelection Message::skipTextSelection(TextSelection selection) const {
	if (selection.from == 0xFFFF || !hasVisibleText()) {
		return selection;
	}
	return HistoryView::UnshiftItemSelection(selection, text());
}

TextSelection Message::unskipTextSelection(TextSelection selection) const {
	if (!hasVisibleText()) {
		return selection;
	}
	return HistoryView::ShiftItemSelection(selection, text());
}

QRect Message::innerGeometry() const {
	auto result = countGeometry();
	if (!hasOutLayout()) {
		const auto w = std::max(
			(media() ? media()->resolveCustomInfoRightBottom().x() : 0),
			result.width());
		result.setWidth(std::min(
			w + rightActionSize().value_or(QSize(0, 0)).width() * 2,
			width()));
	}
	if (hasBubble()) {
		const auto cut = [&](int amount) {
			amount = std::min(amount, result.height());
			result.setTop(result.top() + amount);
		};
		cut(st::msgPadding.top() + st::mediaInBubbleSkip);

		if (displayFromName()) {
			// See paintFromName().
			cut(st::msgNameFont->height);
		}
		if (displayedTopicButton()) {
			cut(st::topicButtonSkip
				+ st::topicButtonPadding.top()
				+ st::msgNameFont->height
				+ st::topicButtonPadding.bottom()
				+ st::topicButtonSkip);
		}
		if (!displayFromName() && !displayForwardedFrom()) {
			// See paintViaBotIdInfo().
			if (data()->Has<HistoryMessageVia>()) {
				cut(st::msgServiceNameFont->height);
			}
		}
		// Skip displayForwardedFrom() until there are no animations for it.
		if (const auto reply = Get<Reply>()) {
			// See paintReplyInfo().
			cut(reply->height());
		}
	}
	return result;
}

QPoint Message::mediaTopLeft() const {
	return _lastMediaPosition;
}

bool Message::isCommentsRootView() const {
	return context() == Context::Replies
		&& data()->isDiscussionPost()
		&& !data()->history()->isForum();
}

QRect Message::countGeometry() const {
	const auto item = data();
	const auto centeredView = item->isFakeAboutView()
		|| isCommentsRootView();
	const auto media = this->media();
	const auto mediaDisplayed = media && media->isDisplayed();
	const auto mediaWidth = mediaDisplayed ? media->width() : width();
	const auto outbg = hasOutLayout();
	const auto useMoreSpace = (delegate()->elementChatMode()
		== ElementChatMode::Narrow);
	const auto wideSkip = useMoreSpace
		? st::msgMargin.left()
		: st::msgMargin.right();
	const auto availableWidth = width()
		- st::msgMargin.left()
		- (centeredView ? st::msgMargin.left() : wideSkip);
	auto contentLeft = hasRightLayout() ? wideSkip : st::msgMargin.left();
	auto contentWidth = availableWidth;
	if (hasFromPhoto()) {
		contentLeft += st::msgPhotoSkip;
		if (const auto size = rightActionSize()) {
			contentWidth -= size->width() + (st::msgPhotoSkip - st::historyFastShareSize);
		}
	//} else if (!Adaptive::Wide() && !out() && !fromChannel() && st::msgPhotoSkip - (hmaxwidth - hwidth) > 0) {
	//	contentLeft += st::msgPhotoSkip - (hmaxwidth - hwidth);
	}
	accumulate_min(contentWidth, maxWidth());
	accumulate_min(contentWidth, int(_bubbleWidthLimit));
	if (mediaWidth < contentWidth) {
		const auto textualWidth = bubbleTextualWidth();
		if (mediaWidth < textualWidth
			&& (!media || !media->enforceBubbleWidth())) {
			accumulate_min(contentWidth, textualWidth);
		} else {
			contentWidth = mediaWidth;
		}
	} else if (!mediaDisplayed) {
		const auto appearing = Get<TextAppearing>();
		const auto use = (appearing && appearing->use)
			? appearing->shownWidth
			: textRealWidth();
		if (use > 0) {
			const auto shrunk = std::max(
				use + st::msgPadding.left() + st::msgPadding.right(),
				int(_nonTextMaxWidth));
			accumulate_min(contentWidth, shrunk);
		}
	}
	if (contentWidth < availableWidth
		&& delegate()->elementChatMode() != ElementChatMode::Wide) {
		if (outbg) {
			contentLeft += availableWidth - contentWidth;
		} else if (centeredView) {
			contentLeft += (availableWidth - contentWidth) / 2;
		}
	} else if (contentWidth < availableWidth && centeredView) {
		contentLeft += std::max(
			((st::msgMaxWidth + 2 * st::msgPhotoSkip) - contentWidth) / 2,
			0);
	}

	const auto contentTop = marginTop();
	return QRect(
		contentLeft,
		contentTop,
		contentWidth,
		height() - contentTop - marginBottom());
}

Ui::BubbleRounding Message::countMessageRounding() const {
	const auto smallTop = isBubbleAttachedToPrevious();
	const auto smallBottom = isBubbleAttachedToNext();
	const auto media = smallBottom ? nullptr : this->media();
	const auto item = data();
	const auto keyboard = item->inlineReplyKeyboard();
	const auto skipTail = smallBottom
		|| (media && media->skipBubbleTail())
		|| (keyboard != nullptr)
		|| item->isFakeAboutView()
		|| isCommentsRootView();
	const auto right = hasRightLayout();
	using Corner = Ui::BubbleCornerRounding;
	return Ui::BubbleRounding{
		.topLeft = (smallTop && !right) ? Corner::Small : Corner::Large,
		.topRight = (smallTop && right) ? Corner::Small : Corner::Large,
		.bottomLeft = ((smallBottom && !right)
			? Corner::Small
			: (!skipTail && !right)
			? Corner::Tail
			: Corner::Large),
		.bottomRight = ((smallBottom && right)
			? Corner::Small
			: (!skipTail && right)
			? Corner::Tail
			: Corner::Large),
	};
}

Ui::BubbleRounding Message::countBubbleRounding(
		Ui::BubbleRounding messageRounding) const {
	if ([[maybe_unused]] const auto _ = data()->inlineReplyKeyboard()) {
		messageRounding.bottomLeft
			= messageRounding.bottomRight
			= Ui::BubbleCornerRounding::Small;
	}
	return messageRounding;
}

Ui::BubbleRounding Message::countBubbleRounding() const {
	return countBubbleRounding(countMessageRounding());
}

int Message::resizeContentGetHeight(int newWidth) {
	invalidateTopicButtonNameRepaint();
	invalidateTopicButtonRippleRepaint();
	if (isHidden()) {
		return marginTop() + marginBottom();
	} else if (newWidth < st::msgMinWidth) {
		return height();
	}

	const auto item = data();
	const auto postShowingAuthor = item->isPostShowingAuthor() ? 1 : 0;
	if (_postShowingAuthor != postShowingAuthor) {
		_postShowingAuthor = postShowingAuthor;
		_fromNameVersion = 0;
		previousInBlocksChanged();

		const auto size = _bottomInfo.currentSize();
		_bottomInfo.update(BottomInfoDataFromMessage(this), newWidth);
		if (size != _bottomInfo.currentSize()) {
			// maxWidth may have changed, full recount required.
			setPendingResize();
			return resizeGetHeight(newWidth);
		}
	}

	const auto media = this->media();
	const auto mediaDisplayed = media ? media->isDisplayed() : false;
	const auto bubble = drawBubble();

	item->resolveDependent();

	// This code duplicates countGeometry() but also resizes media.
	const auto centeredView = item->isFakeAboutView()
		|| isCommentsRootView();
	const auto useMoreSpace = (delegate()->elementChatMode()
		== ElementChatMode::Narrow);
	const auto wideSkip = useMoreSpace
		? st::msgMargin.left()
		: st::msgMargin.right();
	auto contentWidth = newWidth
		- st::msgMargin.left()
		- (centeredView ? st::msgMargin.left() : wideSkip);
	if (hasFromPhoto()) {
		if (const auto size = rightActionSize()) {
			contentWidth -= size->width() + (st::msgPhotoSkip - st::historyFastShareSize);
		}
	}
	accumulate_min(contentWidth, maxWidth());
	_bubbleWidthLimit = (UnlimitedMessageWidth.value() && !mediaDisplayed)
		? 0x3FFFFFF
		: std::max({
			st::msgMaxWidth,
			monospaceMaxWidth(),
			mediaDisplayed ? media->bubbleWidthLimit() : 0,
		});
	accumulate_min(contentWidth, int(_bubbleWidthLimit));
	const auto textualWidth = bubbleTextualWidth();
	if (mediaDisplayed) {
		media->resizeGetHeight(contentWidth);
		if (media->width() < contentWidth) {
			if (media->width() < textualWidth
				&& !media->enforceBubbleWidth()) {
				accumulate_min(contentWidth, textualWidth);
			} else {
				contentWidth = media->width();
			}
		}
	}
	if (!mediaDisplayed && bubble && hasVisibleText()) {
		const auto probeTextWidth = bubbleTextWidth(contentWidth);
		[[maybe_unused]] const auto probe = textHeightFor(probeTextWidth);
		if (!Get<TextAppearing>()) {
			const auto use = textRealWidth();
			if (use > 0) {
				const auto shrunk = std::max(
					use + st::msgPadding.left() + st::msgPadding.right(),
					int(_nonTextMaxWidth));
				accumulate_min(contentWidth, shrunk);
			}
		}
	}
	const auto bottomInfoWidth = qMax(
		contentWidth - st::msgPadding.left() - st::msgPadding.right(),
		1);
	const auto textWidth = bubble
		? bubbleTextWidth(contentWidth)
		: bottomInfoWidth;

	auto appearing = Get<TextAppearing>();
	if (appearing) {
		if (appearing->textWidth != textWidth) {
			appearing->geometryValid = false;
			appearing->textWidth = textWidth;
		}
		// This may invalidate composer structure by removing TextAppearing.
		if (!textAppearValidate(appearing)) {
			appearing = nullptr;
		}
	}

	const auto reactionsInBubble = _reactions && embedReactionsInBubble();
	const auto bottomInfoHeight = _bottomInfo.resizeGetHeight(
		std::min(
			_bottomInfo.maxWidth(),
			bottomInfoWidth - 2 * st::msgDateDelta.x()));

	auto newHeight = minHeight();

	if (const auto service = Get<ServicePreMessage>()) {
		service->resizeToWidth(newWidth, delegate()->elementChatMode());
	}

	const auto botTop = item->isFakeAboutView()
		? Get<FakeBotAboutTop>()
		: nullptr;
	const auto ephemeralBadge = Get<EphemeralBadge>();
	if (bubble) {
		auto reply = Get<Reply>();
		auto via = item->Get<HistoryMessageVia>();
		const auto check = factcheckBlock();
		const auto entry = logEntryOriginal();

		// Entry page is always a bubble bottom.
		auto mediaOnBottom = (mediaDisplayed && media->isBubbleBottom()) || check || (entry/* && entry->isBubbleBottom()*/);
		auto mediaOnTop = (mediaDisplayed && media->isBubbleTop()) || (entry && entry->isBubbleTop());

		if (reactionsInBubble) {
			_reactions->resizeGetHeight(textWidth);
		}
		if (contentWidth == maxWidth() && !appearing) {
			if (mediaDisplayed) {
				newHeight += media->height() - media->minHeight();
				if (check) {
					newHeight += check->resizeGetHeight(contentWidth) + st::mediaInBubbleSkip;
				}
				if (entry) {
					newHeight += entry->resizeGetHeight(contentWidth);
				}
			} else {
				if (check) {
					check->resizeGetHeight(contentWidth);
				}
				if (entry) {
					// In case of text-only message it is counted in minHeight already.
					entry->resizeGetHeight(contentWidth);
				}
			}
		} else {
			const auto withVisibleText = hasVisibleText();
			newHeight = 0;
			if (withVisibleText) {
				if (botTop) {
					newHeight += botTop->height;
				}
				if (appearing) {
					newHeight += appearing->shownHeight;
				} else {
					newHeight += textHeightFor(textWidth);
				}
			}
			if (!mediaOnBottom && (!_viewButton || !reactionsInBubble)) {
				newHeight += st::msgPadding.bottom();
				if (mediaDisplayed) {
					newHeight += st::mediaInBubbleSkip;
				}
			}
			if (!mediaOnTop) {
				newHeight += st::msgPadding.top();
				if (mediaDisplayed) newHeight += st::mediaInBubbleSkip;
				if (entry) newHeight += st::mediaInBubbleSkip;
			}
			if (mediaDisplayed) {
				newHeight += media->height();
			}
			if (check) {
				newHeight += check->resizeGetHeight(contentWidth) + st::mediaInBubbleSkip;
			}
			if (entry) {
				newHeight += entry->resizeGetHeight(contentWidth);
			}
			if (reactionsInBubble) {
				if (mediaDisplayed
					&& !media->additionalInfoString().isEmpty()) {
					// In round videos in a web page status text is painted
					// in the bottom left corner, reactions should be below.
					newHeight += st::msgDateFont->height;
				} else {
					newHeight += st::mediaInBubbleSkip;
				}
				newHeight += _reactions->height();
			}
		}

		if (displayFromName()) {
			fromNameUpdated(contentWidth);
			newHeight += st::msgNameFont->height;
		} else if (via && !displayForwardedFrom()) {
			via->resize(contentWidth - st::msgPadding.left() - st::msgPadding.right());
			newHeight += st::msgNameFont->height;
		}

		if (ephemeralBadge) {
			newHeight += ephemeralBadge->height;
		}

		if (displayedTopicButton()) {
			newHeight += st::topicButtonSkip
				+ st::topicButtonPadding.top()
				+ st::msgNameFont->height
				+ st::topicButtonPadding.bottom()
				+ st::topicButtonSkip;
		}

		if (displayForwardedFrom()) {
			const auto forwarded = item->Get<HistoryMessageForwarded>();
			const auto skip1 = forwarded->psaType.isEmpty()
				? 0
				: st::historyPsaIconSkip1;
			const auto fwdheight = ((forwarded->text.maxWidth() > (contentWidth - st::msgPadding.left() - st::msgPadding.right() - skip1)) ? 2 : 1) * st::semiboldFont->height;
			newHeight += fwdheight;
		}

		if (reply) {
			newHeight += reply->resizeToWidth(contentWidth
				- st::msgPadding.left()
				- st::msgPadding.right());
		}
		if (const auto summaryHeader = Get<SummaryHeader>()) {
			newHeight += summaryHeader->resizeToWidth(contentWidth
				- st::msgPadding.left()
				- st::msgPadding.right());
		}
		if (needInfoDisplay()) {
			newHeight += (bottomInfoHeight - st::msgDateFont->height);
		}

		if (item->repliesAreComments() || item->externalReply()) {
			newHeight += st::historyCommentsButtonHeight;
		} else if (_comments) {
			_comments = nullptr;
			checkHeavyPart();
		}
		newHeight += viewButtonHeight();
	} else if (mediaDisplayed) {
		newHeight = media->height();
	} else {
		newHeight = 0;
	}
	if (_reactions && !reactionsInBubble) {
		const auto reactionsWidth = (!bubble && mediaDisplayed)
			? media->contentRectForReactions().width()
			: contentWidth;
		newHeight += st::mediaInBubbleSkip
			+ _reactions->resizeGetHeight(reactionsWidth);
		if (hasRightLayout()) {
			_reactions->flipToRight();
		}
	}

	if (const auto keyboard = item->inlineReplyKeyboard()) {
		const auto keyboardHeight = st::msgBotKbButton.margin + keyboard->naturalHeight();
		newHeight += keyboardHeight;
		keyboard->resize(contentWidth, keyboardHeight - st::msgBotKbButton.margin);
	}

	newHeight += marginTop() + marginBottom();
	return newHeight;
}

void Message::invalidateTextDependentCache() {
	_bubbleTextualWidthMinimum = -1;
	_bubbleTextualWidthCache = 0;
}

bool Message::textAppearValidate(not_null<TextAppearing*> appearing) {
	while (true) {
		if (!textAppearCheckLine(appearing)) {
			return false;
		} else if (!appearing->use
			|| appearing->widthAnimation.animating()
			|| appearing->heightAnimation.animating()) {
			return true;
		}
		++appearing->shownLine;
		appearing->revealedLineWidth = 0;
		appearing->startLineWidth = 0;
		appearing->targetLineWidth = 0;
	}
}

bool Message::textAppearCheckLine(not_null<TextAppearing*> appearing) {
	const auto recount = !appearing->geometryValid;
	if (recount) {
		appearing->geometryValid = true;
		validateText();
		if (const auto rich = richpage()) {
			const auto articleWidth = richPageWidthFor(appearing->textWidth);
			appearing->lines = rich->article.countRevealLinesGeometry(
				articleWidth);
			if (appearing->lines.empty()) {
				const auto height = textHeightFor(appearing->textWidth);
				if (height > 0) {
					appearing->lines.push_back({
						.left = 0,
						.width = std::max(textRealWidth(), 1),
						.bottom = height,
						.rtl = false,
						.baseline = height,
					});
				}
			}
		} else {
			appearing->lines = text().countLinesGeometry(
				appearing->textWidth);
			auto &lines = appearing->lines;
			if (lines.size() > 1 && text().hasSkipBlock()) {
				const auto &last = lines.back();
				const auto &prev = lines[lines.size() - 2];
				if (last.width == skipBlockWidth()
					&& last.bottom - prev.bottom == skipBlockHeight()) {
					const auto bottom = last.bottom;
					lines.pop_back();
					lines.back().bottom = bottom;
				}
			}
		}
	}
	const auto lines = int(appearing->lines.size());
	const auto shown = appearing->shownLine;
	const auto line = (shown < lines) ? &appearing->lines[shown] : nullptr;
	const auto finalLineHeight = line
		? (hasRichPage() && text().hasSkipBlock() && (shown + 1 == lines)
			? line->bottom + skipBlockHeight()
			: line->bottom)
		: 0;
	const auto use = appearing->use = !anim::Disabled()
		&& line
		&& ((shown + 1 < lines)
			|| (shown + 1 == lines
				&& ((appearing->revealedLineWidth < line->width)
					|| (appearing->shownHeight < finalLineHeight))));
	if (!use) {
		if (data()->isRegular()) {
			// We are inside these animations' tick, can't destroy them now.
			crl::on_main(this, [=] {
				if (Has<TextAppearing>() && !Get<TextAppearing>()->use) {
					RemoveComponents(TextAppearing::Bit());
				}
			});
			return false;
		} else if (recount && lines) {
			appearing->shownLine = lines - 1;
			const auto &line = appearing->lines.back();
			appearing->revealedLineWidth = line.width;
			appearing->shownWidth = textRealWidth();
			appearing->shownHeight = line.bottom
				+ ((hasRichPage() && text().hasSkipBlock())
					? skipBlockHeight()
					: 0);
			appearing->widthAnimation.stop();
			appearing->heightAnimation.stop();
		}
		return true;
	}
	if (appearing->targetLineWidth != line->width) {
		if (appearing->revealedLineWidth >= line->width) {
			appearing->widthAnimation.stop();
			appearing->revealedLineWidth
				= appearing->targetLineWidth
				= line->width;
		} else {
			textAppearStartWidthAnimation(appearing);
		}
	}
	const auto targetHeight = textAppearTargetHeight(appearing);
	if (appearing->targetHeight != targetHeight) {
		if (!shown) {
			appearing->shownHeight = appearing->lines.front().bottom
				+ (appearing->lines.size() > 1 ? skipBlockHeight() : 0);
		}
		if (appearing->shownHeight >= targetHeight) {
			appearing->heightAnimation.stop();
			appearing->shownHeight = appearing->targetHeight = targetHeight;
		} else {
			const auto widthStart = appearing->startLineWidth;
			const auto widthTarget = appearing->targetLineWidth;
			const auto width = appearing->revealedLineWidth;
			const auto progress = (width >= widthTarget)
				? 1.
				: (width - widthStart) / float64(widthTarget - widthStart);
			const auto left = (1. - progress) * appearing->widthDuration;
			const auto duration = appearing->finalizing
				? kLineHeightAppearFinalDuration
				: kLineHeightAppearDuration;
			if (appearing->heightAnimation.animating()
				|| !appearing->widthAnimation.animating()
				|| left <= duration) {
				textAppearStartHeightAnimation(appearing, targetHeight);
			}
		}
	}
	return true;
}

void Message::textAppearStartWidthAnimation(
		not_null<TextAppearing*> appearing) {
	Expects(appearing->use);

	const auto shown = appearing->shownLine;
	const auto lines = int(appearing->lines.size());
	const auto lineWidth = appearing->lines[shown].width;
	const auto lineDuration = appearing->finalizing
		? kFullLineAppearFinalDuration
		: kFullLineAppearDuration;
	const auto computed = (shown + 1 == lines)
		? lineDuration
		: std::max(
			lineDuration * lineWidth / st::msgMaxWidth,
			crl::time(10));
	const auto duration = std::exchange(appearing->startedForText, true)
		? computed
		: std::max(computed, kMinWidthAppearDuration);
	appearing->widthDuration = duration;
	const auto from
		= appearing->startLineWidth
		= appearing->revealedLineWidth;
	const auto to = appearing->targetLineWidth = lineWidth;

	Assert(from < to);
	appearing->widthAnimation.start([=] {
		textAppearWidthCallback();
	}, from, to, duration, anim::linear);
}

void Message::textAppearStartHeightAnimation(
		not_null<TextAppearing*> appearing,
		int targetHeight) {
	Expects(appearing->use);

	const auto from = appearing->shownHeight;
	const auto to = appearing->targetHeight = targetHeight;
	const auto duration = appearing->finalizing
		? kLineHeightAppearFinalDuration
		: kLineHeightAppearDuration;
	appearing->heightAnimation.start([=] {
		textAppearHeightCallback();
	}, from, to, duration, anim::easeOutCubic);
}

int Message::textAppearTargetHeight(
		not_null<TextAppearing*> appearing) const {
	const auto next = appearing->shownLine + 1;
	const auto lines = int(appearing->lines.size());
	if (next + 1 >= lines) {
		return appearing->lines.back().bottom
			+ ((hasRichPage() && text().hasSkipBlock())
				? skipBlockHeight()
				: 0);
	}
	const auto &line = appearing->lines[next];
	const auto bottom = line.bottom;
	const auto rich = hasRichPage();
	const auto nextWidth = rich ? RevealLineRight(line) : line.width;
	const auto available = std::max(
		(rich
			? RevealLineRight(appearing->lines[appearing->shownLine])
			: appearing->lines[appearing->shownLine].width),
		appearing->shownWidth);
	if (nextWidth + skipBlockWidth() <= available && !line.rtl) {
		return bottom;
	}
	return bottom + skipBlockHeight();
}

void Message::textAppearWidthCallback() {
	const auto appearing = Get<TextAppearing>();
	const auto now = int(base::SafeRound(
		appearing->widthAnimation.value(appearing->targetLineWidth)));
	if (now != appearing->revealedLineWidth) {
		appearing->revealedLineWidth = now;
		const auto &line = appearing->lines[appearing->shownLine];
		if (line.rtl) {
			appearing->shownWidth = textRealWidth();
		} else {
			const auto shownWidth = hasRichPage()
				? line.left + now + skipBlockWidth()
				: now + skipBlockWidth();
			appearing->shownWidth = std::min(
				std::max(
					appearing->shownWidth,
					shownWidth),
				textRealWidth());
		}
		repaint();
	}
	textAppearValidate(appearing);
}

void Message::textAppearHeightCallback() {
	const auto appearing = Get<TextAppearing>();
	const auto now = int(base::SafeRound(
		appearing->heightAnimation.value(appearing->targetHeight)));
	if (const auto delta = now - appearing->shownHeight) {
		appearing->shownHeight = now;
		adjustHeight(delta);
		history()->viewHeightAdjusted(this, delta);
		repaint();
	}
	textAppearValidate(appearing);
}

bool Message::needInfoDisplay() const {
	const auto media = this->media();
	const auto mediaDisplayed = media ? media->isDisplayed() : false;
	const auto check = factcheckBlock();
	const auto entry = logEntryOriginal();
	return entry
		? !entry->customInfoLayout()
		: check
		? !check->customInfoLayout()
		: ((mediaDisplayed && media->isBubbleBottom())
			? !media->customInfoLayout()
			: true);
}

bool Message::invertMedia() const {
	return _invertMedia;
}

bool Message::hasVisibleText() const {
	const auto textItem = this->textItem();
	if (!textItem) {
		return false;
	}
	const auto media = this->media();
	if (hasRichPage()) {
		return !media || !media->hideMessageText();
	} else if (textItem->emptyText()) {
		if (const auto media = textItem->media()) {
			return media->storyExpired() || media->storyUnsupported();
		}
		return false;
	}
	return !media || !media->hideMessageText();
}

int Message::visibleTextLength() const {
	return hasVisibleText() ? text().length() : 0;
}

int Message::visibleMediaTextLength() const {
	const auto media = this->media();
	return (media && media->isDisplayed())
		? media->fullSelectionLength()
		: 0;
}

QSize Message::performCountCurrentSize(int newWidth) {
	const auto newHeight = resizeContentGetHeight(newWidth);

	return { newWidth, newHeight };
}

void Message::refreshInfoSkipBlock(HistoryItem *textItem) {
	const auto media = this->media();
	const auto hasTextSkipBlock = [&] {
		if (!textItem || textItem->_text.empty()) {
			if (const auto media = data()->media()) {
				return media->storyExpired() || media->storyUnsupported();
			}
			return false;
		} else if (factcheckBlock()
			|| data()->Has<HistoryMessageLogEntryOriginal>()) {
			return false;
		} else if (media && media->isDisplayed() && !_invertMedia) {
			return false;
		} else if (_reactions) {
			return false;
		}
		return true;
	}();
	const auto skipWidth = skipBlockWidth();
	const auto skipHeight = skipBlockHeight();
	if (_reactions) {
		if (needInfoDisplay()) {
			_reactions->updateSkipBlock(skipWidth, skipHeight);
		} else {
			_reactions->removeSkipBlock();
		}
	}
	validateTextSkipBlock(hasTextSkipBlock, skipWidth, skipHeight);
}

TimeId Message::displayedEditDate() const {
	const auto item = data();
	const auto overrided = media() && media()->overrideEditedDate();
	if (item->hideEditedBadge() && !overrided) {
		return TimeId(0);
	} else if (const auto edited = displayedEditBadge()) {
		return edited->date;
	}
	return TimeId(0);
}

HistoryMessageEdited *Message::displayedEditBadge() {
	if (const auto media = this->media()) {
		if (media->overrideEditedDate()) {
			return media->displayedEditBadge();
		}
	}
	return data()->Get<HistoryMessageEdited>();
}

const HistoryMessageEdited *Message::displayedEditBadge() const {
	if (const auto media = this->media()) {
		if (media->overrideEditedDate()) {
			return media->displayedEditBadge();
		}
	}
	return data()->Get<HistoryMessageEdited>();
}

void Message::ensureSummarizeButton() const {
	if (data()->canBeSummarized()
		/*&& item->originalText().text.size() >= kSummarizeThreshold*/) {
		if (!_summarize) {
			_summarize
				= std::make_unique<TranscribeButton>(
					this,
					data(),
					false,
					true);
		}
	} else {
		_summarize = nullptr;
	}
}

void Message::paintSummarize(
		Painter &p,
		int x,
		int y,
		bool right,
		const PaintContext &context,
		QRect g) const {
	if (!_summarize) {
		return;
	}
	const auto s = _summarize->size();
	const auto bottomY = y - s.height() - st::msgDateImgDelta;
	if (bottomY < g.top()) {
		return;
	}
	const auto topY = g.top();
	const auto buttonY = std::min(
		std::max(topY, context.area.y() + st::msgDateImgDelta),
		bottomY);
	_summarize->paint(
		p,
		x - (right ? 0 : s.width()),
		buttonY,
		context);
}

} // namespace HistoryView
