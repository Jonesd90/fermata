#pragma once
#include "AppContext.h"
#include "../core/TakeSheet.h"
#include "../core/FileSync.h"

#ifndef FERMATA_VERSION_STRING
 #define FERMATA_VERSION_STRING "dev"
#endif

namespace td
{
/** Writes the take sheet (PDF and CSV) of one take window - or of every take window when 'windowId' is null - into the project's "Take Sheets"
    folder, and offers to open it. */
inline void exportTakeSheet (AppContext& app, const juce::Uuid& windowId)
{
    auto& P = app.project;
    const auto sections = takesheet::collect (P, windowId);
    int takes = 0; for (auto& s : sections) takes += (int) s.rows.size();
    if (takes == 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Take sheet", windowId.isNull() ? "No takes have been recorded in this project yet." : "No takes have been recorded in this take window yet.");
        return;
    }
    auto folder = (P.projectFolder() != juce::File() ? P.projectFolder() : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)).getChildFile ("Take Sheets");
    if (! folder.createDirectory()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Take sheet", "Could not make the folder " + folder.getFullPathName()); return; }
    const auto what = windowId.isNull() ? P.name : (P.findTakeWindow (windowId) != nullptr ? P.findTakeWindow (windowId)->name : juce::String ("Takes"));
    const auto base = juce::File::createLegalFileName (what + " - take sheet " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
    const auto pdfFile = folder.getChildFile (base + ".pdf"), csvFile = folder.getChildFile (base + ".csv");
    const auto doc = takesheet::makePdf (sections, P.name, FERMATA_VERSION_STRING, juce::Time::getCurrentTime());
    const bool okPdf = doc.save (pdfFile);
    juce::MemoryOutputStream csvData;
    csvData.write ("\xEF\xBB\xBF", 3);                                       // UTF-8 with a byte order mark: Excel then reads the accents right
    csvData << takesheet::csv (sections);
    const bool okCsv = filesync::writeDataChecked (csvFile, csvData.getData(), csvData.getDataSize());     // (3.1.3: checked)
    // the take chart (3.10.0): in the PDF after each piece's takes, and its runs of bars as a CSV of its own (only when any bars are entered)
    const auto chart = takesheet::chartCsv (sections);
    const auto chartFile = folder.getChildFile (juce::File::createLegalFileName (what + " - take chart " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d")) + ".csv");
    bool okChart = true;
    if (chart.isNotEmpty())
    {
        juce::MemoryOutputStream chartData;
        chartData.write ("\xEF\xBB\xBF", 3);
        chartData << chart;
        okChart = filesync::writeDataChecked (chartFile, chartData.getData(), chartData.getDataSize());
    }
    if (! okPdf || ! okCsv || ! okChart)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Take sheet", "Could not write " + (! okPdf ? pdfFile : ! okCsv ? csvFile : chartFile).getFullPathName() + " (is it open in another program?)");
        return;
    }
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle ("Take sheet")
                                      .withMessage (juce::String (takes) + (takes == 1 ? " take" : " takes") + " on " + juce::String (doc.pageCount()) + (doc.pageCount() == 1 ? " page" : " pages")
                                                    + " (with the take chart), saved as\n" + pdfFile.getFullPathName() + "\nand, for a spreadsheet,\n" + csvFile.getFileName()
                                                    + (chart.isNotEmpty() ? "\n" + chartFile.getFileName() : juce::String()))
                                      .withButton ("Open the PDF").withButton ("Show the folder").withButton ("OK"),
                                  [pdfFile] (int r)
                                  {
                                      if (r == 1) pdfFile.startAsProcess();
                                      else if (r == 2) pdfFile.revealToUser();
                                  });
}
} // namespace td
