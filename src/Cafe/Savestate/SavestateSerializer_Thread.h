#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// ============================================================================
// HISTORIQUE (v1->v4, voir le .cpp pour le detail complet du
// raisonnement verifie) : le seul point reellement bloquant etait
// OSWaitCond, qui deverrouillait un mutex de facon inconditionnelle,
// non idempotente, et perdait prevMutexLockCount (variable locale C++)
// a chaque parcage.
//
// v5 (celle-ci) : CORRIGE PAR UN VRAI PATCH MOTEUR, pas par un
// contournement cote savestate. Voir
// PATCH_coreinit_Synchronization_OSWaitCond.txt : OSWaitCond devient
// idempotent (guard sur mutex->owner == currentThread) et
// prevMutexLockCount est deplace en memoire invitee (OSCond::
// prevMutexLockCount, repurposant le champ ukn08 confirme inutilise
// partout ailleurs dans le depot).
//
// IMPORTANT : OSWAITCOND_PATCH_APPLIED ci-dessous DOIT rester `false`
// tant que ce patch moteur n'est pas reellement applique et compile.
// Le mettre a `true` sans avoir applique le patch reintroduit
// silencieusement la corruption documentee dans l'historique de ce
// fichier (double-deverrouillage, prevMutexLockCount errone).
// ============================================================================

// Mettre a true UNIQUEMENT apres avoir applique
// PATCH_coreinit_Synchronization_OSWaitCond.txt (struct OSCond modifiee
// + OSWaitCond remplacee) et verifie que ça compile.
constexpr bool OSWAITCOND_PATCH_APPLIED = true; // patch applied directly in this repo - see coreinit_Synchronization.cpp OSWaitCond + coreinit_Thread.h OSCond::prevMutexLockCount

namespace SavestateSerializer_Thread
{
	// Avec le patch applique (OSWAITCOND_PATCH_APPLIED == true), renvoie
	// toujours true - OSWaitCond est desormais rejouable sans danger.
	// Sans le patch, conserve le garde-fou verifie precedemment (refuse
	// si un thread est parque dans OSWaitCond specifiquement).
	bool CanSafelySave();

	void DoState(Serializer& s);
	void RecreateHostThreadsAfterPause();
}
