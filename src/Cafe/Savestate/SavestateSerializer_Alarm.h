#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// ============================================================================
// VÉRIFIÉ sur coreinit_Alarm.h réel :
//
// OSAlarm_t (l'API jeu : OSCreateAlarm/OSSetAlarm) est entièrement en
// mémoire invitée (magic, MPTR prev/next, nextTime/period/startTime en
// ticks invités déjà abstraits par OSGetTime() - voir coreinit_Time.cpp).
// Déjà capturé par SavestateSerializer_MMU - RIEN à faire ici pour cette
// API.
//
// La seule chose réellement hôte est `class OSHostAlarm` (opaque,
// déclarée seulement) créée via OSHostAlarmCreate()/OSHostAlarmDestroy().
// Utilisation confirmée : UNIQUEMENT par OSWaitEventWithTimeout() dans
// coreinit_Synchronization.cpp (ligne ~103), comme minuteur de réveil
// anticipé pendant qu'un thread attend un OSEvent.
//
// Je n'ai pas vu la définition de la classe OSHostAlarm elle-même
// (seulement sa déclaration avant), donc je ne sais pas avec certitude
// si son état (la liste des alarmes hôtes en attente, gérée par
// quel mécanisme - une heap séparée ? une liste triée ?) est
// facilement reconstructible. Plutôt que de deviner, ce fichier REFUSE
// la sauvegarde si un thread est actuellement dans cet état précis -
// voir CanSafelySave() ci-dessous. C'est un cas limite rare (une fenêtre
// de quelques millisecondes à quelques secondes selon le timeout), pas
// un obstacle de conception.
// ============================================================================

namespace SavestateSerializer_Alarm
{
	// Refuses if any live thread is currently parked inside
	// OSWaitEventWithTimeout() (i.e. depends on a live OSHostAlarm).
	// Needs a small addition to coreinit_Synchronization.cpp to expose
	// this check - see PATCH_coreinit_Synchronization.txt.
	bool CanSafelySave();

	// No-op given the analysis above - kept as a marker for section
	// ordering and as the natural place to add real OSHostAlarm support
	// later if the timeout-alarm edge case needs to be lifted.
	void DoState(Serializer& s);
}
