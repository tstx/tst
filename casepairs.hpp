#pragma once
#include <cctype>
#include <iostream>
#include <string>

namespace tst {

// 13.10.2026 Interview question (ingenics munich)

// This problem is about removing pairs of characters from a string
// where the characters are the same letter but different cases (e.g., 'a' and
// 'A'). The goal is to remove such pairs iteratively until no more pairs can be
// removed.

// my initial solution, could be more optimal
void remove_case_pairs_inline(std::string &str) {
  for (int i = 0; i < str.size() - 1;
       i++) { // bug: size()-1 underflows for empty string
    char c1 = str[i];
    char c2 = str[i + 1];
    if (std::tolower(c1) == std::tolower(c2)) {
      if (c1 != c2) {
        str.erase(i, 2);
        i -= 2; // -2 needed: after i++ we re-check the new pair (i-1, i)
        if (i < 0)
          i = 0; // bug: will skip str[0] due to i++ in the loop
                 // (i = -1 won't fix it: int/size_t comparison makes -1 huge)
      }
    }
  }
}

// better solution, no erase and it's O(n)
std::string remove_case_pairs_optimal(const std::string &str) {
  std::string out;
  out.reserve(str.size());
  for (char c : str) {
    if (!out.empty() && out.back() != c &&
        std::tolower(static_cast<unsigned char>(out.back())) ==
            std::tolower(static_cast<unsigned char>(c))) {
      out.pop_back(); // Paar wie "aA" oder "Dd" entfernen
    } else {
      out.push_back(c);
    }
  }
  return out;
}


void testCasePairs() {
  std::string test{"aAbcdDEefgHiIjkAaaB"};
  std::cout << "CasePairs: \t'" << test << "'" << std::endl;
  {
    std::string result = test;
    remove_case_pairs_inline(result);
    std::cout << "Inline: \t'" << result << "'" << std::endl;
  }
  {
    std::string result = remove_case_pairs_optimal(test);
    std::cout << "Optimal: \t'" << result << "'" << std::endl;
  }
}

} // namespace tst
