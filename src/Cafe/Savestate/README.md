# Savestates CEmulador — état de l'intégration

Ce dossier et les modifications listées ci-dessous ont été appliqués
directement dans ce dépôt (pas de patch à part à appliquer soi-même).

## Modifications apportées en dehors de Cafe/Savestate/

- `util/ChunkedHeap/ChunkedHeap.h` : `VHeap` reçoit
  `GetAllocatedRangesForSavestate()`/`ResetForSavestate()`/
  `RestoreAllocatedRangesFromSavestate()` ; `ChunkedFlatAllocator<T>`
  reçoit `GetAllocatedBlocksForSavestate()`/`RestoreForSavestate()`.
- `Cafe/OS/libs/coreinit/coreinit_Thread.h/.cpp` :
  `Savestate_RecreateHostThreadsForActiveThreads()` /
  `Savestate_GetActiveThreadList()` ajoutées au namespace `coreinit`.
- `Cafe/OS/libs/coreinit/coreinit_Thread.h` : `OSCond::ukn08` renommé en
  `prevMutexLockCount` (même taille de structure, champ confirmé
  inutilisé ailleurs dans tout le dépôt avant ce changement).
- `Cafe/OS/libs/coreinit/coreinit_Synchronization.cpp` : `OSInitCond`
  et `OSWaitCond` modifiées pour rendre `OSWaitCond` idempotent (sûr à
  rejouer après restauration d'une savestate) — voir le commentaire
  "SAVESTATE SAFETY" dans `OSWaitCond`.
- `Cafe/HW/Latte/Core/Latte.h`, `LatteThread.cpp`,
  `LatteCommandProcessor.cpp` : nouveau mécanisme de pause légère du
  thread GPU (`Latte_RequestPause`/`Latte_WaitForPause`/
  `Latte_ResumePause`), distinct de `Latte_Stop`/`Latte_Start` qui
  détruisent et réinitialisent tout le renderer — inadapté à une pause
  de savestate. Inclut aussi
  `Latte_RequestCacheInvalidationWhilePaused()`, qui fait exécuter
  l'invalidation des caches de textures/FBO **sur le thread GPU
  lui-même** (seul propriétaire valide du contexte GL, même lorsqu'il
  est en pause).
- `Cafe/CMakeLists.txt` : les 20 fichiers de `Cafe/Savestate/` ajoutés
  à la liste de sources.

## Rechargement des modules RPL (résolu, vérifié sur le code)

- Les modules sont rechargés via le mécanisme existant
  (`RPLLoader_RestoreDependencyForSavestate()` + un seul
  `RPLLoader_UpdateDependencies()`), donc cafeLibs / HLE / dossier du titre
  sont gérés exactement comme au boot. Plus aucun point d'intégration FSC manuel.
- `RPLLoader_UnloadAll()` ne libère PAS les régions data/loaderInfo
  (`skipPPCCalls=true`, voir le commentaire MP10 dans rpl.cpp) : les trois
  tas sont donc remis à zéro explicitement avant le rechargement, puis
  l'adresse de chaque module rechargé est comparée à celle sauvegardée ;
  en cas d'écart le chargement est REFUSÉ (sinon les pointeurs déjà présents
  dans la mémoire invitée restaurée seraient corrompus).
- Les IDs HLE embarqués dans les trampolines invités dépendent de l'ordre
  du premier enregistrement. Les callables créés à la volée
  (`RPLLoader_MakePPCCallable`) sont sauvegardés comme décalage par rapport
  à une fonction d'ancrage du même binaire et la table HLE est restaurée aux
  mêmes indices. Conséquence : une savestate n'est chargeable qu'avec le
  MÊME binaire compilé (empreinte de disposition ajoutée au hash de
  compatibilité).
- `hleEntrypointCalled` / `entrypointCalled` sont restaurés, sinon un
  `RPLLoader_CallEntrypoints()` ultérieur relancerait coreinit/GX2.

## Ce qui reste à faire manuellement

1. **Menu et raccourcis** : `src/gui/MainWindow.cpp/.h` et
   `src/gui/input/HotkeySettings.cpp/.h` n'ont pas été modifiés — à
   brancher sur `SavestateManager::RequestSave/RequestLoad(slotIndex)`
   et `RequestSaveToFile/RequestLoadFromFile`.
2. **Miniature** : `SavestateManager.cpp::DoSave()` laisse le buffer de
   miniature vide (`std::vector<uint8> thumbnail;`) — à remplir avec une
   capture d'écran encodée en PNG si souhaité.
3. **Vérification de compilation** : l'intégralité de ce module a été
   écrite et vérifiée ligne par ligne contre le code réel de ce dépôt,
   mais n'a jamais été compilée (pas de toolchain disponible dans cet
   environnement). Une première compilation fera probablement
   remonter quelques erreurs mineures de syntaxe ou d'inclusion
   manquante.

## Garde-fous de sécurité actifs

- Refuse de sauvegarder/charger si `ActiveSettings::IsOnlineEnabled()`
  est vrai (meilleur signal disponible ; ce n'est pas un état de
  session live, voir le commentaire dans `SavestateManager.cpp`).
- Refuse de sauvegarder si un module RPL est en cours de link
  (`SavestateSerializer_RPL::CanSafelySave()`).
- `SavestateSerializer_Thread::CanSafelySave()` : avec le patch
  `OSWaitCond` appliqué (`OSWAITCOND_PATCH_APPLIED = true` dans
  `SavestateSerializer_Thread.h`, déjà fait dans ce dépôt), ce
  garde-fou ne refuse plus jamais rien — il reste en place pour
  pouvoir repasser à `false` facilement si jamais le patch
  `OSWaitCond` devait être retiré ou révisé.

## Points non vérifiés à surveiller au premier test

- `RPLLoader_UpdateDependencies()` appelle `rplHLEModule->RPLMapped()` pour
  chaque module HLE ; je n'ai pas audité si ce callback a des effets de bord
  gênants lorsqu'il est rappelé en pleine partie.
- Le chargement n'a de sens que dans une session où le MÊME titre est déjà
  lancé (le dossier du titre doit être monté pour retrouver les .rpx/.rpl).

## Temps invité, alarmes, table HLE (ajouts finaux)

- Le temps invité (`_tickSummary` de PPCTimer) est sauvegardé et restauré avec la mémoire,
  car la mémoire invité contient des dates absolues (alarmes, timeouts).
- Les alarmes invité actives (`g_activeAlarms`, côté hôte) sont sauvegardées (adresse, prochaine
  échéance, période) et recréées au chargement. Les timeouts des appels bloquants
  (`OSSleepTicks`, `OSWaitEventWithTimeout`) sont recréés avec leur durée complète quand
  l'appel est rejoué.
- La sauvegarde est refusée si l'alarm thread exécute un callback invité (réessayer).
- Toute la table HLE (index -> fonction hôte, stockée en décalage depuis une fonction d'ancrage),
  les exports appelables et les imports mappés sont sauvegardés ; les index mis en cache
  (table des fonctions OS, GamePatch) sont recalculés au chargement.
- Raccourcis par défaut : Shift+F1..F10 = sauvegarder, Ctrl+F1..F10 = charger (modifiables
  dans Options > Raccourcis). Menu : Savestates.
- Limites connues : un savestate n'est valable qu'avec exactement le même binaire ; la
  détection « en ligne » n'est qu'un réglage ; jamais compilé par l'auteur de cette modification.

## Ajouts de la dernière passe

- Section IOSU (id 8) : handles IPC ouverts par le jeu, clients FSA (dossier courant), fichiers et dossiers ouverts
  (chemin, mode, position). Un état n'est accepté que si aucune requête IOSU n'est en vol (réessai automatique).
- Rechargement des modules : désactivé (`SAVESTATE_ALLOW_MODULE_RELOAD`). Si le jeu en cours a déjà les mêmes modules
  que le state, rien n'est déchargé ; sinon le chargement est refusé avec un message clair.
- Le curseur de l'allocateur de la zone système (`sysAreaAllocatorOffset`) est sauvegardé avec la mémoire.
- Fichier : sections compressées en zstd, écriture atomique (fichier .tmp puis renommage), dossier créé au besoin.
- Les refus transitoires (module en cours de chargement, OSWaitCond, callback d'alarme, requête IOSU) sont réessayés
  pendant 3 secondes avant d'être signalés.
- Non couverts : états hôte des autres modules HLE (boss, nsyshid, nfp/ntag, audio AX, clients IOSU autres que FSA).
