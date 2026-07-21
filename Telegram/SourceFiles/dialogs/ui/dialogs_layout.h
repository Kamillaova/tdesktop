/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "dialogs/ui/dialogs_quick_action_context.h"
#include "ui/cached_round_corners.h"

#include <QtGui/QRegion>

#include <optional>

namespace style {
struct DialogRow;
struct VerifiedBadge;
} // namespace style

namespace st {
extern const style::DialogRow &defaultDialogRow;
} // namespace st

namespace Data {
class Forum;
class Folder;
class Thread;
class CommunityInfo;
} // namespace Data

namespace Ui::Text {
class String;
struct CustomEmojiRepaintBounds;
} // namespace Ui::Text

namespace Dialogs {
class Row;
class FakeRow;
class BasicRow;
struct RightButton;
} // namespace Dialogs

namespace Dialogs::Ui {

using namespace ::Ui;

class VideoUserpic;

struct TopicJumpCorners {
	CornersPixmaps normal;
	CornersPixmaps inverted;
	QPixmap small;
	int invertedRadius = 0;
	int smallKey = 0; // = `-radius` if top right else `radius`.
};

struct TopicJumpCache {
	TopicJumpCorners corners;
	TopicJumpCorners over;
	TopicJumpCorners selected;
	TopicJumpCorners rippleMask;
};

struct PaintContext {
	RightButton *rightButton = nullptr;
	std::vector<QImage*> *chatsFilterTags = nullptr;
	QuickActionContext *quickActionContext = nullptr;
	not_null<const style::DialogRow*> st;
	TopicJumpCache *topicJumpCache = nullptr;
	Data::Folder *folder = nullptr;
	Data::Forum *forum = nullptr;
	Data::CommunityInfo *community = nullptr;
	required<QBrush> currentBg;
	FilterId filter = 0;
	float64 topicsExpanded = 0.;
	crl::time now = 0;
	QStringView searchLowerText;
	int width = 0;
	bool active = false;
	bool selected = false;
	bool topicJumpSelected = false;
	bool paused = false;
	bool search = false;
	bool narrow = false;
	bool displayUnreadInfo = false;
	bool insideCommunity = false;
};

struct RowPaintResult {
	QRegion animated;
	std::optional<QRegion> quickActionAnimation;
	uint64 animationGeneration = 0;
	bool messagePreviewPainted = false;
};

[[nodiscard]] QRegion TextAnimationRegion(
	const Text::String &text,
	QRect spoilerGeometry,
	const Text::CustomEmojiRepaintBounds &customEmojiRepaintBounds,
	QRect customEmojiFallback);

extern const char kOptionDialogsMuteIcon[];

[[nodiscard]] const style::icon *ChatTypeIcon(
	not_null<PeerData*> peer,
	const PaintContext &context);
[[nodiscard]] const style::icon *ChatTypeIcon(not_null<PeerData*> peer);

[[nodiscard]] const style::VerifiedBadge &VerifiedStyle(
	const PaintContext &context);

class RowPainter {
public:
	static RowPaintResult Paint(
		Painter &p,
		not_null<const Row*> row,
		VideoUserpic *videoUserpic,
		const PaintContext &context);
	static RowPaintResult Paint(
		Painter &p,
		not_null<const FakeRow*> row,
		const PaintContext &context);
	static QRect SendActionAnimationRect(
		not_null<const Data::Thread*> thread,
		FilterId filterId,
		QRect rect,
		int fullWidth,
		bool textUpdated);
};

void PaintCollapsedRow(
	Painter &p,
	const BasicRow &row,
	Data::Folder *folder,
	const QString &text,
	int unread,
	const PaintContext &context);

int PaintRightButton(QPainter &p, const PaintContext &context);

} // namespace Dialogs::Ui
