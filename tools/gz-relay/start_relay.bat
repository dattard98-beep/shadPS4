@echo off
REM SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Starts the Godzilla relay and keeps the DuckDNS name updated.
REM Put your DuckDNS name and token below, save, then double-click this file.

set DUCKDNS_DOMAIN=skibidirizzlin
set DUCKDNS_TOKEN=PASTE-YOUR-TOKEN-HERE

title Godzilla Relay
cd /d "%~dp0"
python gz_relay.py --port 3659 --duckdns-domain %DUCKDNS_DOMAIN% --duckdns-token %DUCKDNS_TOKEN%
echo.
echo The relay stopped. Press any key to close.
pause >nul
