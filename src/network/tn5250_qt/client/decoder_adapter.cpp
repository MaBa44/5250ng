// 5250ng - A modern IBM TN5250 terminal emulator
// Copyright (C) 2025-2026 Remi GASCOU (Podalirius)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "decoder_adapter.h"

#include <tn5250/protocol_constants.h>

#include <algorithm>
#include <cstring>

namespace tn5250::client {

namespace {

// Advance index `i` past one non-WTD ESC command starting at rec[i] == ESC,
// mirroring the protocol decoder's resync rules. Returns the new index,
// clamped to `end`.
int skipEscCommand(const uint8_t *rec, int i, int end) {
    using namespace tn5250::protocol;
    // rec[i] == ESC, need at least the command byte
    if (i + 1 >= end)
        return end;
    const uint8_t cc = rec[i + 1];
    if (cc == CC_CLEAR_UNIT || cc == CC_CLEAR_UNIT_ALTERNATE || cc == CC_CLEAR_FORMAT_TABLE)
        return std::min(i + 2, end);
    if (cc == CC_READ_INPUT_FIELDS || cc == CC_READ_MDT_FIELDS || cc == CC_READ_IMMEDIATE
        || cc == CC_READ_SCREEN || cc == CC_READ_SCREEN_ALT)
        return std::min(i + 4, end);
    if (cc == CC_WRITE_ERROR_CODE || cc == CC_WRITE_ERROR_CODE_TO_WINDOW) {
        int k = i + 2;
        while (k < end && rec[k] != ESC)
            k++;
        return k;
    }
    if (cc == CC_ROLL) {
        if (i + 3 >= end)
            return end;
        if (i + 5 < end)
            return i + 6;
        return i + 4;
    }
    if (cc == CC_WRITE_STRUCTURED_FIELD) {
        // Length-prefixed structured fields; binary payload may contain ESC.
        int j = i + 2;
        while (j + 1 < end) {
            if (rec[j] == ESC)
                break;
            const int sfLen = (rec[j] << 8) | rec[j + 1];
            if (sfLen < 2 || j + sfLen > end)
                return end;
            j += sfLen;
        }
        return j;
    }
    // Unknown command: resync at the next ESC like the protocol decoder.
    int k = i + 2;
    while (k < end && rec[k] != ESC)
        k++;
    return k;
}

// Skip one Start Field order at rec[k] == ORDER_SF, mirroring the protocol
// decoder: FFW1/FFW2, FCW pairs, attribute byte, 2-byte field length.
int skipSfOrder(const uint8_t *rec, int k, int end) {
    int m = k + 1;
    if (m + 1 < end)
        m += 2;
    else
        return end;
    while (m + 1 < end && (rec[m] & 0xE0) != 0x20)
        m += 2;
    if (m < end)
        m++;
    if (m + 1 < end)
        m += 2;
    return std::min(m, end);
}

} // namespace

void DecoderAdapter::parseData(const QByteArray &data) {
    extractWdsfOrders(data);
    m_decoder.parseData(fromQt(data));
}

void DecoderAdapter::reset() {
    m_decoder.reset();
    m_pending.clear();
}

void DecoderAdapter::extractWdsfOrders(const QByteArray &data) {
    using namespace tn5250::protocol;
    m_pending.append(data);
    const uint8_t *rec = reinterpret_cast<const uint8_t *>(m_pending.constData());
    while (m_pending.size() >= 2) {
        rec = reinterpret_cast<const uint8_t *>(m_pending.constData());
        const int recLen = (rec[0] << 8) | rec[1];
        if (recLen < GDS_MIN_RECORD_LEN) {
            m_pending.remove(0, 1);
            continue;
        }
        if (m_pending.size() < recLen)
            return; // wait for the rest of the record
        if (recLen >= 7 && rec[2] == GDS_RECORD_TYPE_HI && rec[3] == GDS_RECORD_TYPE_LO) {
            const int varLen = rec[6];
            if (varLen >= 4 && 6 + 3 < recLen) {
                const uint8_t opcode = rec[6 + 3];
                if (opcode == GDS_OPCODE_OUTPUT_ONLY || opcode == GDS_OPCODE_PUT_GET
                    || opcode == GDS_OPCODE_RESTORE || opcode == GDS_OPCODE_SAVE_SCREEN
                    || opcode == GDS_OPCODE_READ_SCREEN)
                    scanRecordPayloadForWdsf(rec, 6 + varLen, recLen);
            }
        }
        m_pending.remove(0, recLen);
    }
}

void DecoderAdapter::scanRecordPayloadForWdsf(const uint8_t *rec, int start, int end) {
    using namespace tn5250::protocol;
    int i = start;
    while (i < end) {
        if (rec[i] != ESC) {
            i++;
            continue;
        }
        if (i + 1 >= end)
            break;
        const uint8_t cc = rec[i + 1];
        if (cc != CC_WRITE_TO_DISPLAY) {
            if (cc == SOH) {
                // SOH can also appear at top level: SOH + 1 length byte.
                if (i + 1 >= end)
                    break;
                i = std::min(i + 2 + rec[i + 1], end);
                continue;
            }
            i = skipEscCommand(rec, i, end);
            continue;
        }
        // Write-To-Display command: walk orders until the next ESC.
        if (i + 3 >= end)
            break;
        int cursorRow = -1;
        int cursorCol = -1;
        int j = i + 4;
        while (j < end) {
            const uint8_t ob = rec[j];
            if (ob == ESC)
                break;
            if (ob == SOH) {
                if (j + 1 >= end) {
                    j = end;
                    break;
                }
                j = std::min(j + 2 + rec[j + 1], end);
                continue;
            }
            if (ob == ORDER_SBA || ob == ORDER_IC || ob == ORDER_MC) {
                if (j + 2 < end) {
                    cursorRow = rec[j + 1] - 1;
                    cursorCol = rec[j + 2] - 1;
                }
                j = std::min(j + 3, end);
                continue;
            }
            if (ob == ORDER_RA || ob == ORDER_EA) {
                j = std::min(j + 4, end);
                continue;
            }
            if (ob == ORDER_TD) {
                // Single length byte, mirroring the protocol decoder.
                if (j + 1 >= end) {
                    j = end;
                    break;
                }
                j = std::min(j + 2 + rec[j + 1], end);
                continue;
            }
            if (ob == ORDER_WDSF) {
                if (j + 2 >= end) {
                    j = end;
                    break;
                }
                int wdsfLen = (rec[j + 1] << 8) | rec[j + 2];
                if (wdsfLen < 2)
                    wdsfLen = 2;
                const int wdsfEnd = std::min(j + 1 + wdsfLen, end);
                emit wdsfOrderReceived(
                    QByteArray(reinterpret_cast<const char *>(rec + j), wdsfEnd - j),
                    cursorRow, cursorCol);
                j = wdsfEnd;
                continue;
            }
            if (ob == ORDER_SF) {
                j = skipSfOrder(rec, j, end);
                continue;
            }
            if (ob == STRPCCMD_TRIGGER && j + STRPCCMD_MARKER_LEN <= end
                && std::memcmp(rec + j + 1, STRPCCMD_SIGNATURE, sizeof(STRPCCMD_SIGNATURE)) == 0) {
                int p = j + STRPCCMD_MARKER_LEN;
                if (p < end && rec[p] != ESC) {
                    p++;
                    const int cmdEnd = std::min(p + STRPCCMD_MAX_COMMAND_LEN, end);
                    while (p < cmdEnd && rec[p] >= 0x40)
                        p++;
                }
                j = p;
                continue;
            }
            j++; // display data byte
        }
        i = j;
    }
}

static QByteArray toQt(const std::vector<uint8_t> &v) {
    return QByteArray(reinterpret_cast<const char *>(v.data()), static_cast<int>(v.size()));
}

static std::vector<uint8_t> fromQt(const QByteArray &ba) {
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(ba.constData()),
                                 reinterpret_cast<const uint8_t *>(ba.constData()) + ba.size());
}

DecoderAdapter::DecoderAdapter(QObject *parent) : QObject(parent) {
    auto &cb = m_decoder.callbacks();

    cb.onCommandReceived = [this](TN5250Command cmd, const std::vector<uint8_t> &data) {
        emit commandReceived(cmd, toQt(data));
    };
    cb.onStructuredFieldReceived = [this](StructuredFieldType type, const std::vector<uint8_t> &data) {
        emit structuredFieldReceived(type, toQt(data));
    };
    cb.onRawScreenData = [this](const std::vector<uint8_t> &data) {
        emit rawScreenDataReceived(toQt(data));
    };
    cb.onClearScreen = [this]() { emit clearScreenRequested(); };
    cb.onClearScreenAlternate = [this]() { emit clearScreenAlternateRequested(); };
    cb.onKeyboardUnlock = [this]() { emit keyboardUnlockRequested(); };
    cb.onControlCharacters = [this](uint8_t cc1, uint8_t cc2) {
        emit controlCharactersReceived(cc1, cc2);
    };
    cb.onSoh = [this](uint8_t er, uint8_t c1, uint8_t c2, uint8_t c3) {
        emit sohReceived(er, c1, c2, c3);
    };
    cb.onRoll = [this](uint8_t top, uint8_t bot, uint8_t lines, bool up) {
        emit rollRequested(top, bot, lines, up);
    };
    cb.onWriteErrorCode = [this](const std::vector<uint8_t> &data) {
        emit writeErrorCodeRequested(toQt(data));
    };
    cb.onSaveScreen = [this]() { emit saveScreenRequested(); };
    cb.onClearFormatTable = [this]() { emit clearFormatTableRequested(); };
    cb.onInvite = [this]() { emit inviteReceived(); };
    cb.onCancelInvite = [this]() { emit cancelInviteReceived(); };
    cb.onMessageLightOn = [this]() { emit messageLightOn(); };
    cb.onMessageLightOff = [this]() { emit messageLightOff(); };
    cb.onReadScreen = [this](bool attrs) { emit readScreenRequested(attrs); };
    cb.onWriteStructuredField = [this](const std::vector<uint8_t> &data) {
        emit writeStructuredFieldReceived(toQt(data));
    };
    cb.onStrpccmdRequested = [this](bool noWait, const std::vector<uint8_t> &cmd) {
        emit strpccmdRequested(noWait, toQt(cmd));
    };
    cb.onParseError = [this](const std::string &err) {
        emit parseError(QString::fromStdString(err));
    };
}

} // namespace tn5250::client
