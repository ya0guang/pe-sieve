#include "pattern_finder.h"
#include <peconv.h>

sig_ma::SigFinder signFinder;
bool isSignInit = false;

void initSigFinders()
{
	std::cout << "Init signs\n";
	// 32 bit
	signFinder.loadSignature("prolog32_1", "55 8b ec");
	signFinder.loadSignature("prolog32_2", "55 89 e5");
	signFinder.loadSignature("prolog32_3", "60 89 ec");

	// 64 bit
	signFinder.loadSignature("prolog64_1", "40 53 48 83 ec");
	signFinder.loadSignature("prolog64_2", "55 48 8B EC");
	signFinder.loadSignature("prolog64_3", "40 55 48 83 EC");

	signFinder.loadSignature("prolog64_4", "53 48 81 EC");
	signFinder.loadSignature("prolog64_5", "48 83 E4 f0");
	signFinder.loadSignature("prolog64_6", "57 48 89 E7");

	signFinder.loadSignature("prolog64_7", "48 8B C4 48 89 58 08 4C 89 48 20 4C 89 40 18 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57");
	isSignInit = true;
}

sig_ma::matched_set pesieve::find_matching_patterns(const BYTE* loadedData, size_t loadedSize, bool stopOnFirstMatch)
{
	if (!loadedSize || peconv::is_padding(loadedData, loadedSize, 0)) {
		return sig_ma::matched_set();
	}
	if (!isSignInit) {
		initSigFinders();
	}
	return signFinder.getMatching(loadedData, loadedSize, 0, sig_ma::FRONT_TO_BACK, stopOnFirstMatch);
}

#define  USE_SIG_FINDER
bool pesieve::fill_matching(const BYTE* loadedData, size_t loadedSize, size_t startOffset, MatchesInfo& _matchesInfo)
{
	if (!loadedData || !loadedSize) return false;

#ifdef  USE_SIG_FINDER
	sig_ma::matched_set allMatched = find_matching_patterns(loadedData, loadedSize, false);
	for (auto itr = allMatched.matchedSigns.begin(); itr != allMatched.matchedSigns.end(); ++itr) {
		const sig_ma::matched match = *itr;

		for (auto mItr = match.signs.begin(); mItr != match.signs.end(); ++mItr) {
			sig_ma::PckrSign* sign = *mItr;
			if (!sign) continue;

			pesieve::util::t_pattern_matched m;
			m.offsets.insert(match.match_offset + startOffset);
			m.name = sign->getName();
			_matchesInfo.appendMatch(m);
		}
	}
	return allMatched.size() > 0 ? true : false;
#else

	bool is64 = false;

	pesieve::util::t_pattern_matched matched = util::find_32bit_code(loadedData, loadedSize);
	bool found = false;
	if (matched.offset != PATTERN_NOT_FOUND) {
		found = true;
	}
	if (!found) {
		matched = util::find_64bit_code(loadedData, loadedSize);
		if (matched.offset != PATTERN_NOT_FOUND) {
			found = true;
			is64 = true;
		}
	}
	_matchesInfo.appendMatch(matched);
#ifdef _DEBUG
	if (found) {
		std::cout << "Is64: " << is64 << " Pattern ID: " << pattern_found << "\n";
	}
#endif //_DEBUG
	return found;
#endif //  USE_SIG_FINGER
}