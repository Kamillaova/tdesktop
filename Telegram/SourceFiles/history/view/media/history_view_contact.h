/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/media/history_view_media.h"
#include "ui/userpic_view.h"

namespace Data {
struct SharedContact;
} // namespace Data

namespace Ui {
class EmptyUserpic;
class GenericBox;
class RippleAnimation;
} // namespace Ui

namespace HistoryView {

class Contact final : public Media {
public:
	Contact(
		not_null<Element*> parent,
		const Data::SharedContact &data);
	~Contact();

	void draw(Painter &p, const PaintContext &context) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	bool toggleSelectionByHandlerClick(
			const ClickHandlerPtr &p) const override {
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

	// Should be called only by Data::Session.
	void updateSharedContactUserId(UserId userId) override;

	void unloadHeavyPart() override;
	bool hasHeavyPart() const override;

private:
	struct RippleRepaint {
		QRegion current;
		QRegion stale;
		uint64 generation = 0;
		uint32 pending : 1 = 0;
		uint32 known : 1 = 0;
	};

	QSize countOptimalSize() override;

	void clickHandlerPressedChanged(
		const ClickHandlerPtr &p, bool pressed) override;
	void repaintMainRipple(uint64 generation) const;
	void repaintButtonRipple(int index, uint64 generation) const;
	void repaintRipple(
		RippleRepaint &repaint,
		uint64 generation) const;
	void recordRippleRepaint(
		RippleRepaint &repaint,
		const Painter &p,
		const PaintContext &context,
		QRect rect) const;
	void invalidateRippleRepaint(RippleRepaint &repaint) const;
	void invalidateRippleRepaints() const;
	void repaintRippleRegion(const QRegion &region) const;
	[[nodiscard]] RippleRepaint &buttonRippleRepaint(int index) const;
	[[nodiscard]] uint64 resetRippleRepaint(
		RippleRepaint &repaint) const;

	[[nodiscard]] QMargins inBubblePadding() const;
	[[nodiscard]] QMargins innerMargin() const;
	[[nodiscard]] int bottomInfoPadding() const;
	[[nodiscard]] QRect buttonRect(
		const QRect &inner,
		const QRect &outer,
		int index) const;

	[[nodiscard]] TextSelection toTitleSelection(
		TextSelection selection) const;
	[[nodiscard]] TextSelection toDescriptionSelection(
		TextSelection selection) const;

	[[nodiscard]] bool hasSingleLink() const;

	const style::QuoteStyle &_st;
	const int _pixh;

	UserId _userId = 0;
	UserData *_contact = nullptr;

	Ui::Text::String _nameLine;
	Ui::Text::String _phoneLine;

	Fn<void(not_null<Ui::GenericBox*>)> _vcardBoxFactory;

	struct Button {
		QString text;
		int width = 0;
		ClickHandlerPtr link;
		mutable std::unique_ptr<Ui::RippleAnimation> ripple;
		mutable QSize rippleSize;
	};
	std::vector<Button> _buttons;
	Button _mainButton;
	mutable RippleRepaint _mainRippleRepaint;
	mutable std::vector<RippleRepaint> _buttonRippleRepaints;
	mutable QSize _rippleLayoutSize;
	mutable uint64 _nextRippleGeneration = 0;

	std::unique_ptr<Ui::EmptyUserpic> _photoEmpty;
	mutable Ui::PeerUserpicView _userpic;
	mutable QPoint _lastPoint;

};

} // namespace HistoryView
