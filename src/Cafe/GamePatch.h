void GamePatch_scan();
bool GamePatch_IsNonReturnFunction(uint32 hleIndex);
// Savestate support (see Cafe/Savestate): re-resolves cached HLE indices after the HLE table was replaced.
void GamePatch_RefreshHLEIndicesForSavestate();
