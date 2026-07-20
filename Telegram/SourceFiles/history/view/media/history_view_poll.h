/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/media/history_view_media.h"
#include "ui/effects/animations.h"
#include "data/data_poll.h"
#include "base/weak_ptr.h"
#include "base/timer.h"

#include <QtGui/QRegion>
#include <QtGui/QTransform>

namespace Ui {
class RippleAnimation;
class FireworksAnimation;
} // namespace Ui

namespace HistoryView {

class Message;

class Poll final : public Media {
public:
	Poll(
		not_null<Element*> parent,
		not_null<PollData*> poll,
		const TextWithEntities &consumed);
	~Poll();

	void draw(Painter &p, const PaintContext &context) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	bool toggleSelectionByHandlerClick(const ClickHandlerPtr &p) const override {
		return true;
	}
	bool dragItemByHandler(const ClickHandlerPtr &p) const override {
		return true;
	}

	bool needsBubble() const override {
		return true;
	}
	bool customInfoLayout() const override {
		return false;
	}

	[[nodiscard]] TextSelection adjustSelection(
		TextSelection selection,
		TextSelectType type) const override;
	uint16 fullSelectionLength() const override;
	TextForMimeData selectedText(TextSelection selection) const override;

	BubbleRoll bubbleRoll() const override;
	QMargins bubbleRollRepaintMargins() const override;
	void paintBubbleFireworks(
		Painter &p,
		const QRect &bubble,
		crl::time ms) const override;

	void clickHandlerActiveChanged(
		const ClickHandlerPtr &handler,
		bool active) override;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;

	void hideSpoilers() override;

	void unloadHeavyPart() override;
	bool hasHeavyPart() const override;
	void parentTextUpdated() override;

	[[nodiscard]] QRect addOptionRect(int innerWidth) const override;
	void setAddOptionActive(bool active) override;

private:
	struct Part;
	struct Header;
	struct Options;
	struct AddOption;
	struct Footer;

	struct AnswerAnimation;
	struct AnswersAnimation;
	struct SendingAnimation;
	struct Answer;
	struct AttachedMedia;
	struct SolutionMedia;
	struct RecentVoter;
	struct RepaintState {
		uint64 generation = 0;
		QRegion current;
		QRegion stale;
		uint32 pending : 1 = 0;
		uint32 known : 1 = 0;
	};

	QSize countOptimalSize() override;
	QSize countCurrentSize(int newWidth) override;

	[[nodiscard]] bool showVotes() const;
	[[nodiscard]] PollData::VoteRestriction knownVoteRestriction() const;
	[[nodiscard]] bool voteRestricted() const;
	void showVoteRestrictionToast() const;
	[[nodiscard]] bool canVote() const;
	[[nodiscard]] bool canSendVotes() const;
	[[nodiscard]] bool isAuthorNotVoted() const;
	void updateTexts();
	void updateVotes();
	bool showVotersCount() const;
	bool inlineFooter() const;

	[[nodiscard]] bool canAddOption() const;
	void refreshWebpageSubscriptions();
	void recordRepaintGeometry(
		RepaintState &repaint,
		QRegion region,
		bool known) const;
	void recordRepaintGeometry(
		RepaintState &repaint,
		const Painter &p,
		const PaintContext &context,
		const QRegion &region) const;
	void recordAnimationRepaintGeometry(
		RepaintState &repaint,
		QRegion region,
		bool known) const;
	void invalidateRepaintGeometry(RepaintState &repaint) const;
	void repaintGeometry(RepaintState &repaint) const;
	void repaintGeometry(
		RepaintState &repaint,
		uint64 generation) const;
	[[nodiscard]] uint64 startRepaintGeneration(
		RepaintState &repaint) const;
	void stopRepaintGeneration(RepaintState &repaint) const;
	void repaintRegion(const QRegion &region) const;
	void invalidateFiniteRepaintGeometries() const;
	void rememberElementPaint(
		const Painter &p,
		const PaintContext &context) const;
	[[nodiscard]] std::optional<QRect> mapCurrentPaintToElement(
		const Painter &p,
		QRectF rect) const;

	not_null<PollData*> _poll;
	std::vector<WebPageData*> _registeredWebpages;
	int _pollVersion = 0;
	int _totalVotes = 0;
	bool _voted = false;
	PollData::Flags _flags = PollData::Flags();

	mutable std::unique_ptr<Ui::FireworksAnimation> _fireworksAnimation;
	Ui::Animations::Simple _wrongAnswerAnimation;
	mutable QPoint _lastLinkPoint;
	mutable RepaintState _fireworksRepaint;
	mutable RepaintState _wrongAnswerRepaint;
	mutable uint64 _nextRepaintGeneration = 0;
	mutable QSize _repaintLayoutSize;
	mutable const QPaintDevice *_elementPaintDevice = nullptr;
	mutable std::optional<QTransform> _elementTransform;
	mutable const QPaintDevice *_lastDrawPaintDevice = nullptr;
	mutable uint32 _lastDrawCanonical : 1 = 0;

	bool _addOptionActive = false;
	mutable bool _wrongAnswerAnimated = false;
	mutable bool _adminShowResults = false;

	std::unique_ptr<Header> _headerPart;
	std::unique_ptr<Options> _optionsPart;
	std::unique_ptr<AddOption> _addOptionPart;
	std::unique_ptr<Footer> _footerPart;

};

} // namespace HistoryView
