#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// ============================================================================
// CONFIRME (et non plus suppose) : j'ai lu l'integralite de
// coreinit_Synchronization.cpp (675 lignes) dans le vrai depot. Aucun
// std::mutex/condition_variable/etat hote natif ne sauvegarde quoi que
// ce soit pour OSMutex/OSSemaphore/OSEvent - ce sont des structs
// entierement en memoire invitee (magic, MEMPTR<OSThread_t> owner,
// OSThreadQueue, compteurs), deja captures par SavestateSerializer_MMU.
//
// La seule fonction hote trouvee dans ce fichier est
// OSHostAlarmCreate()/OSHostAlarmDestroy(), utilisee uniquement par
// OSWaitEventWithTimeout() pour le minuteur de timeout - ce n'est PAS de
// l'etat de synchronisation, c'est traite comme un cas particulier dans
// SavestateSerializer_Alarm.cpp (avec un refus explicite de sauvegarder
// si un thread est dans cet etat precis, faute d'avoir verifie le
// support complet).
// ============================================================================

namespace SavestateSerializer_Sync
{
	void DoState(Serializer& s);
}
