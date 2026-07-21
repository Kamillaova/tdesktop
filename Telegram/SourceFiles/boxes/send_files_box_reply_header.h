/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/effects/animations.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

#include <QtGui/QRegion>

class HistoryItem;

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Data {
class Session;
} // namespace Data

namespace Ui {
class IconButton;
class SpoilerAnimation;
} // namespace Ui

namespace SendFiles {

class ReplyPillHeader final : public Ui::RpWidget {
public:
	ReplyPillHeader(
		QWidget *parent,
		std::shared_ptr<ChatHelpers::Show> show,
		FullReplyTo replyTo);
	~ReplyPillHeader();

	[[nodiscard]] rpl::producer<> closeRequests() const;
	[[nodiscard]] rpl::producer<> hideFinished() const;
	[[nodiscard]] rpl::producer<int> desiredHeight() const;

	void setRoundedShapeBelow(bool value);
	void hideAnimated();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	struct AnimationDamage {
		QRegion current;
		QRegion stale;
		QRegion fallback;
		bool known = false;
		bool scheduled = false;
	};

	void resolveMessageData();
	void setShownMessage(HistoryItem *item);
	void updateShownMessageText();
	void textAnimationRepaint();
	void previewSpoilerRepaint();
	void invalidateAnimationDamage(AnimationDamage &damage);
	void recordAnimationDamage(
		AnimationDamage &damage,
		QRegion current,
		QRegion fallback,
		const QRegion &repaintRegion);
	void scheduleAnimationRepaint(
		AnimationDamage &state,
		QRegion damage);
	void animationCallback();

	const std::shared_ptr<ChatHelpers::Show> _show;
	const not_null<Data::Session*> _data;
	const FullReplyTo _replyTo;
	const not_null<Ui::IconButton*> _cancel;

	HistoryItem *_shownMessage = nullptr;
	AnimationDamage _textAnimationDamage;
	AnimationDamage _previewSpoilerDamage;
	Ui::Text::String _shownMessageName;
	Ui::Text::String _shownMessageText;
	std::unique_ptr<Ui::SpoilerAnimation> _previewSpoiler;

	Ui::Animations::Simple _showAnimation;
	rpl::variable<int> _desiredHeight = 0;
	rpl::event_stream<> _closeRequests;
	rpl::variable<bool> _hideFinished = false;
	bool _hiding = false;
	bool _roundedShapeBelow = true;

};

} // namespace SendFiles
