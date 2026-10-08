#pragma once

#include <string>

// Persisted English model selection.
//
// English ships two models, and which one is loaded defines the program's
// mode:
//
//   Normal mode  Medium English + Small Spanish. 3 cores for English,
//                1 for Spanish. More accurate.
//   Potato mode  Small English + Small Spanish. 1 core per language, 2 in
//                total. Ultralight, lower CPU, slightly less accurate.
//
// Spanish always uses Small Streaming: no Medium Spanish model exists.
//
// The choice is asked once on first run, then remembered. The Options menu
// can change it, which requires a restart to take effect. Stored beside the
// executable in olas-model.txt, so a missing or unreadable file simply means
// "not chosen yet".

namespace olas {

enum class EnglishModel {
    Unset,   // never chosen: prompt on startup
    Medium,
    Small,
};

// Reads the saved choice. Returns Unset when the file is missing or malformed.
EnglishModel load_english_model();

// Writes the choice. Returns false if the file could not be written.
bool save_english_model(EnglishModel m);

// "medium" / "small", for logs and the dialog.
const char *english_model_name(EnglishModel m);

// The architecture number for a choice (0 when Unset).
int english_model_arch(EnglishModel m);

// Path of the settings file, for messages.
std::string model_choice_path();

} // namespace olas
