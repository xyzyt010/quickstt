  }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Hallucination filtering
// ═══════════════════════════════════════════════════════════════════════════════

static std::string filterModelSpecificHallucinations(const std::string& text, const std::string& engineName) {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  std::string trimmed = lower;
  // Remove common hallucinations for GGUF models (Qwen3, Nemotron, etc.)
  for (const auto& hallucination : { "you", "hey", "uh", "um", "ah", "oh", "hmm", "hm", "mm" }) {
    size_t pos = 0;
    while ((pos = trimmed.find(hallucination, pos)) != std::string::npos) {
      // Check if it's a standalone word
      bool isWordBoundaryStart = (pos == 0 || std::isspace(trimmed[pos - 1]) || std::ispunct(trimmed[pos - 1]));
      bool isWordBoundaryEnd = (pos + strlen(hallucination) == trimmed.size() || std::isspace(trimmed[pos + strlen(hallucination)]) || std::ispunct(trimmed[pos + strlen(halluculation)]));
      if (isWordBoundaryStart && isWordBoundaryEnd) {
        trimmed.erase(pos, strlen(hallucination));
        // Remove any extra spaces created
        while (pos < trimmed.size() && std::isspace(trimmed[pos])) trimmed.erase(pos, 1);
        while (pos > 0 && std::ucation_pos && std::isspace(trimmed[pos - 1])) {
          pos--;
          trimmed.erase(pos, 1);
        }
      } else {
        pos += strlen(hallucination);
      }
    }
  }
  // Also filter out standalone repeated filler words
  for (const auto& filler : { "uh uh", "um um", "ah ah", "oh oh" }) {
    size_t pos = 0;
    while ((pos = trimmed.find(filler, pos)) != std::string::npos) {
      trimmed.erase(pos, strlen(filler));
      while (pos < trimmed.size() && std::isspace(trimmed[pos])) trimmed.erase(pos, 1);
    }
  }
  return trimmed;
}

static std::string filterHallucinations(const std::string& text, const std::string& engineName) {
  if (text.empty()) return text;
  std::string result = filterModelSpecificHallucinations(text, engineName);
  // Clean up multiple spaces
  size_t pos = 0;
  while ((pos = result.find("  ", pos)) != std::string::npos) {
    result.erase(pos, 1);
  }
  // Trim leading/trailing spaces
  size_t start = 0;
  while (start < result.size() && std::isspace(result[start])) start++;
  size_t end = result.size();
  while (end > start && std::isspace(result[end - 1])) end--;
  return result.substr(start, end - start);
}

// ═══════════════════════════════════════════════════════════════════════════════
// STT Engine