/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "history/view/history_view_object.h"
#include "data/data_message_reaction_id.h"

namespace Data {
class Reactions;
} // namespace Data

namespace Ui {
struct ChatPaintContext;
struct ReactionFlyAnimationArgs;
class ReactionFlyAnimation;
} // namespace Ui

namespace Ui::Text {
class CustomEmoji;
} // namespace Ui::Text

namespace HistoryView {
using PaintContext = Ui::ChatPaintContext;
class Element;
struct TextState;
struct UserpicInRow;
} // namespace HistoryView

namespace HistoryView::Reactions {

using ::Data::ReactionId;
using ::Data::MessageReaction;

struct InlineListData {
	enum class Flag : uchar {
		InBubble  = 0x01,
		OutLayout = 0x02,
		Flipped   = 0x04,
		Tags      = 0x08,
		Centered  = 0x10,
	};
	friend inline constexpr bool is_flag_type(Flag) { return true; };
	using Flags = base::flags<Flag>;

	std::vector<MessageReaction> reactions;
	base::flat_map<ReactionId, std::vector<not_null<PeerData*>>> recent;
	Flags flags = {};
};

class InlineList final : public Object, public base::has_weak_ptr {
public:
	using Data = InlineListData;
	InlineList(
		not_null<::Data::Reactions*> owner,
		Fn<ClickHandlerPtr(ReactionId)> handlerFactory,
		Fn<void(QRect)> customEmojiRepaint,
		Fn<void(QRect)> animationRepaint,
		Data &&data);
	~InlineList();

	void update(Data &&data, int availableWidth);
	QSize countCurrentSize(int newWidth) override;
	[[nodiscard]] int countNiceWidth() const;
	[[nodiscard]] int placeAndResizeGetHeight(QRect available);
	void flipToRight();

	void updateSkipBlock(int width, int height);
	void removeSkipBlock();

	[[nodiscard]] bool areTags() const;
	[[nodiscard]] std::vector<ReactionId> computeTagsList() const;
	[[nodiscard]] bool hasCustomEmoji() const;
	void unloadCustomEmoji();

	void paint(
		Painter &p,
		const PaintContext &context,
		int outerWidth,
		const QRect &clip) const;
	[[nodiscard]] bool getState(
		QPoint point,
		not_null<TextState*> outResult) const;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed);

	void animate(Ui::ReactionFlyAnimationArgs &&args);
	[[nodiscard]] auto takeAnimations()
	-> base::flat_map<
		ReactionId,
		std::unique_ptr<Ui::ReactionFlyAnimation>>;
	void continueAnimations(base::flat_map<
		ReactionId,
		std::unique_ptr<Ui::ReactionFlyAnimation>> animations);

	[[nodiscard]] static float64 TagDotAlpha();
	[[nodiscard]] static QImage PrepareTagBg(QColor tagBg, QColor dotBg);

private:
	struct Dimension {
		int left = 0;
		int width = 0;
	};
	struct Userpics {
		QImage image;
		std::vector<UserpicInRow> list;
		bool someNotLoaded = false;
	};
	struct Button;
	struct CustomEmojiRepaint;
	struct RippleEffect;
	enum class AnimationPart : uchar;

	void layout();
	void layoutButtons();

	void setButtonTag(Button &button, const QString &title);
	void setButtonCount(Button &button, int count);
	void setButtonUserpics(
		Button &button,
		const std::vector<not_null<PeerData*>> &peers);
	[[nodiscard]] Button prepareButtonWithId(const ReactionId &id);
	void resolveUserpicsImage(const Button &button) const;
	void paintCustomFrame(
		Painter &p,
		const Button &button,
		QRect target,
		const PaintContext &context,
		const QColor &textColor) const;
	void customEmojiUpdated(
		const ReactionId &id,
		uint64 generation) const;
	void invalidateCustomEmojiRepaints();
	void syncCustomEmojiRepaints();
	void beginCustomEmojiPaint(
		const Painter &p,
		const PaintContext &context) const;
	void recordCustomEmojiRect(
		const Painter &p,
		const PaintContext &context,
		const Button &button,
		QRect rect) const;
	void finishCustomEmojiPaint() const;
	void repaintCustomEmojiRegion(const QRegion &region) const;
	[[nodiscard]] uint64 startAnimationRepaint(
		Button &button,
		AnimationPart part);
	void stopAnimationRepaint(
		const Button &button,
		AnimationPart part) const;
	void animationUpdated(
		const ReactionId &id,
		AnimationPart part,
		uint64 generation) const;
	void invalidateAnimationRepaints();
	void recordAnimationRepaintRect(
		const Painter &p,
		const PaintContext &context,
		const Button &button,
		QRect rect) const;
	void paintSingleBg(
		Painter &p,
		const QRect &fill,
		const QColor &color,
		float64 opacity) const;

	void validateTagBg(const QColor &color) const;

	QSize countOptimalSize() override;
	[[nodiscard]] Dimension countDimension(int width) const;

	const not_null<::Data::Reactions*> _owner;
	const Fn<ClickHandlerPtr(ReactionId)> _handlerFactory;
	const Fn<void(QRect)> _customEmojiRepaint;
	const Fn<void(QRect)> _animationRepaint;
	Data _data;
	mutable std::vector<CustomEmojiRepaint> _customEmojiRepaints;
	std::vector<Button> _buttons;
	QSize _skipBlock;
	mutable QImage _tagBg;
	mutable QColor _tagBgColor;
	mutable QImage _customCache;
	uint64 _customEmojiGeneration = 0;
	uint64 _animationGeneration = 0;
	bool _hasCustomEmoji = false;
	mutable std::unique_ptr<RippleEffect> _ripple;
	mutable QPoint _lastPoint;
	mutable ReactionId _lastPointButton;

};

[[nodiscard]] InlineListData InlineListDataFromMessage(
	not_null<Element*> view);

[[nodiscard]] ReactionId ReactionIdOfLink(const ClickHandlerPtr &link);

struct ReactionCount {
	int count = 0;
	bool shortened = false;
};
[[nodiscard]] ReactionCount ReactionCountOfLink(
	HistoryItem *item,
	const ClickHandlerPtr &link);

} // namespace HistoryView::Reactions
