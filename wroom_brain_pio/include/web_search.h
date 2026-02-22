#ifndef WEB_SEARCH_H
#define WEB_SEARCH_H

#include <Arduino.h>

// Search result structure
struct SearchResult {
  String title;
  String url;
  String snippet;
};

// Perform web search with automatic fallback (Serper -> Tavily)
// Returns true if search succeeded, false otherwise
// results_out: Array of search results (max 10)
// results_count: Number of results returned
// provider_used: Which provider was used (for debugging)
bool web_search(const String &query, SearchResult *results_out, int *results_count,
               String &provider_used, String &error_out);

// Simple search interface - returns formatted text
bool web_search_simple(const String &query, String &formatted_output, String &error_out);

#endif
