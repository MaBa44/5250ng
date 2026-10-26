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

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <cstdint>

#include <tn5250/client/decoder.h>

namespace tn5250::client {

// Qt adapter around the protocol library's callback-based Decoder.
// Bridges std::function callbacks to Qt signals.
class DecoderAdapter : public QObject {
    Q_OBJECT

  public:
    explicit DecoderAdapter(QObject *parent = nullptr);

    void parseData(const QByteArray &data);
    void reset();
    ParserState state() const { return m_decoder.state(); }

  signals:
    void commandReceived(TN5250Command cmd, const QByteArray &data);
    void structuredFieldReceived(StructuredFieldType type, const QByteArray &data);
    void rawScreenDataReceived(const QByteArray &data);
    void clearScreenRequested();
    void clearScreenAlternateRequested();
    void keyboardUnlockRequested();
    void controlCharactersReceived(uint8_t cc1, uint8_t cc2);
    void sohReceived(uint8_t errorRow, uint8_t cmdKeyMask1, uint8_t cmdKeyMask2, uint8_t cmdKeyMask3);
    void rollRequested(uint8_t topRow, uint8_t botRow, uint8_t lines, bool up);
    void writeErrorCodeRequested(const QByteArray &errorCode);
    void saveScreenRequested();
    void clearFormatTableRequested();
    void inviteReceived();
    void cancelInviteReceived();
    void messageLightOn();
    void messageLightOff();
    void readScreenRequested(bool includeAttributes);
    void writeStructuredFieldReceived(const QByteArray &data);
    // Fires for a Write-To-Display Structured Field order (0x15) found inside
    // a Write-To-Display command. The protocol decoder (protocol-tn5250
    // submodule) strips WDSF orders from the display stream, so this adapter
    // pre-scans complete GDS records and reports the raw order bytes
    // (layout on the wire: 0x15 LEN-hi LEN-lo 0xD9 TYPE ...) before handing
    // the data to the decoder. cursorRow/cursorCol is the host buffer
    // position from the last SBA/MC/IC order in the same WTD command, or
    // -1/-1 when no absolute position precedes the order.
    void wdsfOrderReceived(const QByteArray &order, int cursorRow, int cursorCol);
    // Fires when the host's WTD stream contains the STRPCCMD marker. Carries
    // the wait flag (true → no-wait, EBCDIC 'a' / 0x81) and the raw EBCDIC
    // command bytes consumed by the protocol decoder from immediately after
    // the marker. Codepage conversion and trimming are the consumer's
    // responsibility (TN5250CommandHandler).
    void strpccmdRequested(bool noWait, const QByteArray &commandBytes);
    void parseError(const QString &error);

  private:
    Decoder m_decoder;
    // Unprocessed GDS bytes held back for the WDSF pre-scan: extraction only
    // runs on complete records so orders split across TCP chunks are still
    // reported exactly once with correct framing.
    QByteArray m_pending;

    // Scan complete GDS records in m_pending (plus new data) for WDSF orders
    // and emit wdsfOrderReceived for each one found.
    void extractWdsfOrders(const QByteArray &data);
    // Walk one GDS record payload, emitting wdsfOrderReceived for every
    // WDSF order with the cursor from the last SBA/MC/IC in its WTD command.
    void scanRecordPayloadForWdsf(const uint8_t *rec, int start, int end);
};

} // namespace tn5250::client
