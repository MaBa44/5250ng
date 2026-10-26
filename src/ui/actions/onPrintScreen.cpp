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

#include "../main_window.h"
#include "core/codepage.h"
#include <QFontDatabase>
#include <QPrintDialog>
#include <QPrinter>
#include <QTextDocument>
#include <QtUiStyle/StyledMessageBox.h>

/**
 * Print the current session screen as text (tn5250j "host print screen"
 * parity for the workstation side: the 0x66 Read Screen to Print command
 * already returns the screen image to the host; this action sends the same
 * screen text to a local printer or PDF).
 */
void MainWindow::onPrintScreen() {
    if (m_activeIndex < 0 || m_activeIndex >= m_sessions.size()) {
        qt_ui_style::StyledMessageBox::information(this, "Print Screen", "No active session tab to print.");
        return;
    }
    Session *session = m_sessions[m_activeIndex];
    if (!session->displayWidget || !session->displayWidget->screenBuffer()) {
        qt_ui_style::StyledMessageBox::warning(this, "Print Screen", "No screen buffer to print.");
        return;
    }

    // Convert the EBCDIC screen buffer to Unicode text with the session's
    // code page. Unmapped bytes (controls) become spaces so columns align.
    auto *screen = session->displayWidget->screenBuffer();
    core::CodePage cp(session->config.codePage());
    QStringList lines;
    for (int r = 0; r < screen->rows(); ++r) {
        QString line;
        line.reserve(screen->cols());
        for (int c = 0; c < screen->cols(); ++c) {
            const uint8_t ebcdic = screen->character(r, c);
            QChar ch = cp.toUnicode(ebcdic);
            if (ch.isNull() || ch.category() == QChar::Other_Control)
                ch = QChar(' ');
            line.append(ch);
        }
        // Trim trailing blanks; keep the line itself so rows stay aligned.
        while (!line.isEmpty() && line.back() == QChar(' '))
            line.chop(1);
        lines.append(line);
    }
    while (!lines.isEmpty() && lines.back().isEmpty())
        lines.removeLast();
    if (lines.isEmpty()) {
        qt_ui_style::StyledMessageBox::information(this, "Print Screen", "The screen is empty.");
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(QString("5250ng %1").arg(session->config.name()));
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle("Print Screen");
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QTextDocument doc;
    doc.setDefaultFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    doc.setPlainText(lines.join('\n'));
    doc.print(&printer);
}
