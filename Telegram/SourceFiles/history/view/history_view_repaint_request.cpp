/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_repaint_request.h"

#include "data/data_session.h"
#include "ui/paint/damage.h"

#include <QtGui/QPainter>

namespace HistoryView {

void ViewRepaintMapper::record(const QPainter &p) {
	_transform = p.transform();
	_recorded = true;
}

std::optional<QRegion> ViewRepaintMapper::map(
		const Data::RequestViewRepaint &request) const {
	if (!_recorded
		|| (request.rect.isEmpty() && request.region.isEmpty())) {
		return std::nullopt;
	}
	auto result = QRegion();
	const auto add = [&](QRect rect) {
		result += Ui::DamageRect(QRectF(rect), _transform);
	};
	if (request.region.isEmpty()) {
		add(request.rect);
	} else {
		for (const auto &rect : request.region) {
			add(rect);
		}
	}
	return result;
}

} // namespace HistoryView
