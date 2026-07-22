/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/damage_debug.h"

#include <cstdio>

namespace Ui {

void LogUnknownGeometryRepaint(const char *component) {
	std::fprintf(
		stderr,
		"Damage: unknown geometry fallback in %s.\n",
		component);
	std::fflush(stderr);
}

} // namespace Ui
