#pragma once

#include <string>
#include <vector>

// Reads the positioned text out of simple, machine-generated PDFs: enough for the US House's
// trade reports (EO.Pdf, Type0/Identity-H fonts with ToUnicode maps), not a general PDF reader.
// Objects are found by scanning, so compressed object streams aren't read. Encrypted files are
// read only with the standard handler's RC4 revisions 2-3 and an empty user password.
namespace strojanka {

struct PdfText {
    double y = 0;  // text-space y as drawn (grows downward in EO.Pdf files, and keeps growing across pages)
    double x = 0;
    std::string text;  // UTF-8
};

// Every run of text that starts at a text-matrix move, in drawing order. error is set (and the
// result empty) only when the file can't be read at all.
std::vector<PdfText> pdf_text_fragments(const std::string& pdf, std::string* error);

}  // namespace strojanka
