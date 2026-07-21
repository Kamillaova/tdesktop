/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

#include <QtGui/QRegion>

class HistoryItem;

namespace Data {
class DocumentMedia;
class PhotoMedia;
class Story;
} // namespace Data

namespace Ui {
class SpoilerAnimation;
} // namespace Ui

namespace Info::Statistics {

struct SavedState;

class MessagePreview final : public Ui::RpWidget {
public:
	MessagePreview(
		not_null<Ui::RpWidget*> parent,
		not_null<HistoryItem*> item,
		QImage cachedPreview);
	MessagePreview(
		not_null<Ui::RpWidget*> parent,
		not_null<Data::Story*> story,
		QImage cachedPreview);

	void setInfo(int views, int shares, int reactions);
	void saveState(SavedState &state) const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

	int resizeGetHeight(int newWidth) override;

private:
	struct AnimationDamage {
		QRegion current;
		QRegion stale;
		QRegion fallback;
		bool known = false;
		bool scheduled = false;
	};

	void processPreview();
	void textAnimationRepaint();
	void invalidateTextAnimationDamage();
	void recordTextAnimationDamage(
		QRegion current,
		QRegion fallback,
		const QRegion &repaintRegion);
	void scheduleTextAnimationRepaint(QRegion damage);
	[[nodiscard]] QRect previewRect() const;

	FullMsgId _messageId;
	FullStoryId _storyId;
	AnimationDamage _textAnimationDamage;
	Ui::Text::String _text;
	Ui::Text::String _date;
	Ui::Text::String _views;
	Ui::Text::String _shares;
	Ui::Text::String _reactions;

	int _viewsWidth = 0;
	int _sharesWidth = 0;
	int _reactionsWidth = 0;

	QImage _cornerCache;
	QImage _preview;

	std::shared_ptr<Data::PhotoMedia> _photoMedia;
	std::shared_ptr<Data::DocumentMedia> _documentMedia;
	std::unique_ptr<Ui::SpoilerAnimation> _spoiler;

	rpl::lifetime _lifetimeDownload;

};

} // namespace Info::Statistics
