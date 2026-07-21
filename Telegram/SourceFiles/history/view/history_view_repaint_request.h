/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtGui/QRegion>
#include <QtGui/QTransform>

#include <optional>

class QPainter;

namespace Data {
struct RequestViewRepaint;
} // namespace Data

namespace HistoryView {

class ViewRepaintMapper final {
public:
	void record(const QPainter &p);
	[[nodiscard]] std::optional<QRegion> map(
		const Data::RequestViewRepaint &request) const;

private:
	QTransform _transform;
	bool _recorded = false;

};

} // namespace HistoryView
