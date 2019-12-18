/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod PhysHooks Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"
#include "CDetour/detours.h"
#include <sourcehook.h>
#include <sh_memory.h>
#include <server_class.h>
#include <ispatialpartition.h>

#define SetBit(A,I)		((A)[(I) >> 5] |= (1 << ((I) & 31)))
#define ClearBit(A,I)	((A)[(I) >> 5] &= ~(1 << ((I) & 31)))
#define CheckBit(A,I)	!!((A)[(I) >> 5] & (1 << ((I) & 31)))

class CTriggerMoved : public IPartitionEnumerator
{
public:
	virtual IterationRetval_t EnumElement( IHandleEntity *pHandleEntity ) = 0;
};

class CTouchLinks : public IPartitionEnumerator
{
public:
	virtual IterationRetval_t EnumElement( IHandleEntity *pHandleEntity ) = 0;
};

struct SrcdsPatch
{
	const char *pSignature;
	const unsigned char *pPatchSignature;
	const char *pPatchPattern;
	const unsigned char *pPatch;

	unsigned char *pOriginal;
	uintptr_t pAddress;
	uintptr_t pPatchAddress;
	bool engine;
} gs_Patches[] = {
	// Hook: Replace call inside Physics_RunThinkFunctions
	{
		"_Z25Physics_RunThinkFunctionsb",
		(unsigned char *)"\x8B\x04\x9E\x85\xC0\x74\x13\xA1\x00\x00\x00\x00\x89\x78\x0C\x8B\x04\x9E\x89\x04\x24\xE8\x00\x00\x00\x00",
		"xxxxxxxx????xxxxxxxxxx????",
		NULL,
		0, 0, 0, false
	}
};

uintptr_t FindPattern(uintptr_t BaseAddr, const unsigned char *pData, const char *pPattern, size_t MaxSize);

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

PhysHooks g_Interface;		/**< Global singleton for extension's main interface */

SMEXT_LINK(&g_Interface);

CGlobalVars *gpGlobals = NULL;
IGameConfig *g_pGameConf = NULL;

CDetour *g_pDetour_RunThinkFunctions = NULL;

IForward *g_pOnRunThinkFunctions = NULL;
IForward *g_pOnPrePlayerThinkFunctions = NULL;
IForward *g_pOnPostPlayerThinkFunctions = NULL;
IForward *g_pOnRunThinkFunctionsPost = NULL;

int g_SH_TriggerMoved = 0;
int g_SH_TouchLinks = 0;

CTriggerMoved *g_CTriggerMoved = NULL;
CTouchLinks *g_CTouchLinks = NULL;


DETOUR_DECL_STATIC1(DETOUR_RunThinkFunctions, void, bool, simulating)
{
	if(g_pOnRunThinkFunctions->GetFunctionCount())
	{
		g_pOnRunThinkFunctions->PushCell(simulating);
		g_pOnRunThinkFunctions->Execute();
	}

	if(g_pOnPrePlayerThinkFunctions->GetFunctionCount())
	{
		g_pOnPrePlayerThinkFunctions->Execute();
	}

	DETOUR_STATIC_CALL(DETOUR_RunThinkFunctions)(simulating);

	if(g_pOnRunThinkFunctionsPost->GetFunctionCount())
	{
		g_pOnRunThinkFunctionsPost->PushCell(simulating);
		g_pOnRunThinkFunctionsPost->Execute();
	}
}

void (*g_pPhysics_SimulateEntity)(CBaseEntity *pEntity) = NULL;
void Physics_SimulateEntity_CustomLoop(CBaseEntity **ppList, int Count, float Startime)
{
	CBaseEntity *apPlayers[SM_MAXPLAYERS];
	int iPlayers = 0;

	// Remove players from list and put into apPlayers
	for(int i = 0; i < Count; i++)
	{
		CBaseEntity *pEntity = ppList[i];
		if(!pEntity)
			continue;

		edict_t *pEdict = gamehelpers->EdictOfIndex(gamehelpers->EntityToBCompatRef(pEntity));
		if(!pEdict)
			continue;

		int Entity = gamehelpers->IndexOfEdict(pEdict);
		if(Entity >= 1 && Entity <= SM_MAXPLAYERS)
		{
			apPlayers[iPlayers++] = pEntity;
			ppList[i] = NULL;
		}
	}

	// Shuffle players array
	for(int i = iPlayers - 1; i > 0; i--)
	{
		int j = rand() % (i + 1);
		CBaseEntity *pTmp = apPlayers[j];
		apPlayers[j] = apPlayers[i];
		apPlayers[i] = pTmp;
	}

	// Simulate players first
	for(int i = 0; i < iPlayers; i++)
	{
		gpGlobals->curtime = Startime;
		g_pPhysics_SimulateEntity(apPlayers[i]);
	}

	// Post Player simulation done
	if(g_pOnPostPlayerThinkFunctions->GetFunctionCount())
	{
		g_pOnPostPlayerThinkFunctions->Execute();
	}

	// Now simulate the rest
	for(int i = 0; i < Count; i++)
	{
		CBaseEntity *pEntity = ppList[i];
		if(!pEntity)
			continue;

		gpGlobals->curtime = Startime;
		g_pPhysics_SimulateEntity(pEntity);
	}
}

int g_TriggerEntityMoved;
int *g_pBlockTriggerTouchPlayers = NULL;
int *g_pBlockTriggerMoved = NULL;
// void IVEngineServer::TriggerMoved( edict_t *pTriggerEnt, bool testSurroundingBoundsOnly ) = 0;
SH_DECL_HOOK2_void(IVEngineServer, TriggerMoved, SH_NOATTRIB, 0, edict_t *, bool);
void TriggerMoved(edict_t *pTriggerEnt, bool testSurroundingBoundsOnly)
{
	g_TriggerEntityMoved = gamehelpers->IndexOfEdict(pTriggerEnt);

	// Block if bit is set
	if(g_pBlockTriggerMoved && CheckBit(g_pBlockTriggerMoved, g_TriggerEntityMoved))
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	// Decide per entity in TriggerMoved_EnumElement
	RETURN_META(MRES_IGNORED);
}

// IterationRetval_t CTriggerMoved::EnumElement( IHandleEntity *pHandleEntity ) = 0;
SH_DECL_HOOK1(CTriggerMoved, EnumElement, SH_NOATTRIB, 0, IterationRetval_t, IHandleEntity *);
IterationRetval_t TriggerMoved_EnumElement(IHandleEntity *pHandleEntity)
{
	if(!g_pBlockTriggerTouchPlayers)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	IServerUnknown *pUnk = static_cast< IServerUnknown* >( pHandleEntity );
	CBaseHandle hndl = pUnk->GetRefEHandle();
	int index = hndl.GetEntryIndex();

	// We only care about players
	if(index > SM_MAXPLAYERS)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	// block touching any clients here if bit is set
	if(CheckBit(g_pBlockTriggerTouchPlayers, g_TriggerEntityMoved))
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// allow touch
	RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
}

cell_t BlockTriggerMoved(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockTriggerMoved);
	else
		g_pBlockTriggerMoved = NULL;

	return 0;
}

cell_t BlockTriggerTouchPlayers(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockTriggerTouchPlayers);
	else
		g_pBlockTriggerTouchPlayers = NULL;

	return 0;
}

int g_SolidEntityMoved;
int *g_pBlockSolidMoved = NULL;
int *g_pBlockSolidTouchPlayers = NULL;
int *g_pFilterClientSolidTouch = NULL;
// void IVEngineServer::SolidMoved( edict_t *pSolidEnt, ICollideable *pSolidCollide, const Vector* pPrevAbsOrigin, bool testSurroundingBoundsOnly ) = 0;
SH_DECL_HOOK4_void(IVEngineServer, SolidMoved, SH_NOATTRIB, 0, edict_t *, ICollideable *, const Vector *, bool);
void SolidMoved(edict_t *pSolidEnt, ICollideable *pSolidCollide, const Vector *pPrevAbsOrigin, bool testSurroundingBoundsOnly)
{
	g_SolidEntityMoved = gamehelpers->IndexOfEdict(pSolidEnt);

	// Block if bit is set
	if(g_pBlockSolidMoved && CheckBit(g_pBlockSolidMoved, g_SolidEntityMoved))
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	// Decide per entity in TouchLinks_EnumElement
	RETURN_META(MRES_IGNORED);
}

// IterationRetval_t CTouchLinks::EnumElement( IHandleEntity *pHandleEntity ) = 0;
SH_DECL_HOOK1(CTouchLinks, EnumElement, SH_NOATTRIB, 0, IterationRetval_t, IHandleEntity *);
IterationRetval_t TouchLinks_EnumElement(IHandleEntity *pHandleEntity)
{
	IServerUnknown *pUnk = static_cast< IServerUnknown* >( pHandleEntity );
	CBaseHandle hndl = pUnk->GetRefEHandle();
	int index = hndl.GetEntryIndex();

	// Optimization: Players shouldn't touch other players
	if(g_SolidEntityMoved <= SM_MAXPLAYERS && index <= SM_MAXPLAYERS)
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// block solid from touching any clients here if bit is set
	if(g_pBlockSolidTouchPlayers && index <= SM_MAXPLAYERS && CheckBit(g_pBlockSolidTouchPlayers, g_SolidEntityMoved))
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// Block player from touching any filtered entity here if bit is set
	if(g_pFilterClientSolidTouch && index < 2048 && CheckBit(g_pFilterClientSolidTouch, g_SolidEntityMoved * 2048 + index))
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// Allow otherwise
	RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
}

cell_t BlockSolidMoved(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockSolidMoved);
	else
		g_pBlockSolidMoved = NULL;

	return 0;
}

cell_t BlockSolidTouchPlayers(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockSolidTouchPlayers);
	else
		g_pBlockSolidTouchPlayers = NULL;

	return 0;
}

cell_t FilterClientSolidTouch(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pFilterClientSolidTouch);
	else
		g_pFilterClientSolidTouch = NULL;

	return 0;
}

bool PhysHooks::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	srand((unsigned int)time(NULL));

	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("PhysHooks.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
			snprintf(error, maxlength, "Could not read PhysHooks.games.txt: %s", conf_error);

		return false;
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	g_pDetour_RunThinkFunctions = DETOUR_CREATE_STATIC(DETOUR_RunThinkFunctions, "Physics_RunThinkFunctions");
	if(g_pDetour_RunThinkFunctions == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for Physics_RunThinkFunctions");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_RunThinkFunctions->EnableDetour();

	// Find VTable for CTriggerMoved
	uintptr_t pCTriggerMoved;
	if(!g_pGameConf->GetMemSig("CTriggerMoved", (void **)(&pCTriggerMoved)) || !pCTriggerMoved)
	{
		snprintf(error, maxlength, "Failed to find CTriggerMoved.\n");
		SDK_OnUnload();
		return false;
	}
	// First function in VTable
	g_CTriggerMoved = (CTriggerMoved *)(pCTriggerMoved + 8);

	// Find VTable for CTouchLinks
	uintptr_t pCTouchLinks;
	if(!g_pGameConf->GetMemSig("CTouchLinks", (void **)(&pCTouchLinks)) || !pCTouchLinks)
	{
		snprintf(error, maxlength, "Failed to find CTouchLinks.\n");
		SDK_OnUnload();
		return false;
	}
	// First function in VTable
	g_CTouchLinks = (CTouchLinks *)(pCTouchLinks + 8);

	g_SH_TriggerMoved = SH_ADD_DVPHOOK(CTriggerMoved, EnumElement, g_CTriggerMoved, SH_STATIC(TriggerMoved_EnumElement), false);
	g_SH_TouchLinks = SH_ADD_DVPHOOK(CTouchLinks, EnumElement, g_CTouchLinks, SH_STATIC(TouchLinks_EnumElement), false);

	SH_ADD_HOOK(IVEngineServer, TriggerMoved, engine, SH_STATIC(TriggerMoved), false);
	SH_ADD_HOOK(IVEngineServer, SolidMoved, engine, SH_STATIC(SolidMoved), false);

	if(!g_pGameConf->GetMemSig("Physics_SimulateEntity", (void **)(&g_pPhysics_SimulateEntity)) || !g_pPhysics_SimulateEntity)
	{
		snprintf(error, maxlength, "Failed to find Physics_SimulateEntity.\n");
		SDK_OnUnload();
		return false;
	}

	void *pServerSo = dlopen("cstrike/bin/server_srv.so", RTLD_NOW);
	if(!pServerSo)
	{
		snprintf(error, maxlength, "Could not dlopen server_srv.so");
		SDK_OnUnload();
		return false;
	}

	void *pEngineSo = dlopen("bin/engine_srv.so", RTLD_NOW);
	if(!pEngineSo)
	{
		snprintf(error, maxlength, "Could not dlopen engine_srv.so");
		dlclose(pServerSo);
		SDK_OnUnload();
		return false;
	}

	/* Hook: Replace call inside Physics_RunThinkFunctions */
	uintptr_t pAddress = (uintptr_t)memutils->ResolveSymbol(pServerSo, gs_Patches[0].pSignature);
	if(!pAddress)
	{
		snprintf(error, maxlength, "Could not find symbol: %s", gs_Patches[0].pSignature);
		dlclose(pServerSo);
		dlclose(pEngineSo);
		SDK_OnUnload();
		return false;
	}

	uintptr_t pPatchAddress = FindPattern(pAddress, gs_Patches[0].pPatchSignature, gs_Patches[0].pPatchPattern, 1024);
	if(!pPatchAddress)
	{
		snprintf(error, maxlength, "Could not find patch signature for symbol: %s", gs_Patches[0].pSignature);
		dlclose(pServerSo);
		dlclose(pEngineSo);
		SDK_OnUnload();
		return false;
	}

	// mov [esp+8], edi ; startime
	// mov [esp+4], eax ; count
	// mov [esp], esi ; **list
	// call NULL ; <- our func here
	// mov eax, ds:gpGlobals ; restore
	// jmp +11 ; jump over useless instructions
	static unsigned char aPatch[] = "\x89\x7C\x24\x08\x89\x44\x24\x04\x89\x34\x24\xE8\x00\x00\x00\x00\xA1\x00\x00\x00\x00\xEB\x0B\x90\x90\x90";
	gs_Patches[0].pPatch = aPatch;

	// put our function address into the relative call instruction
	// relative call: new PC = PC + imm1
	// call is at + 11 after pPatchAddress
	// PC will be past our call instruction so + 5
	*(uintptr_t *)&aPatch[12] = (uintptr_t)Physics_SimulateEntity_CustomLoop - (pPatchAddress + 11 + 5);

	// restore "mov eax, ds:gpGlobals" from original to our patch after the call
	*(uintptr_t *)&aPatch[17] = *(uintptr_t *)(pPatchAddress + 8);

	// Apply all patches
	for(size_t i = 0; i < sizeof(gs_Patches) / sizeof(*gs_Patches); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

		void *pBinary = pPatch->engine ? pEngineSo : pServerSo;
		pPatch->pAddress = (uintptr_t)memutils->ResolveSymbol(pBinary, pPatch->pSignature);
		if(!pPatch->pAddress)
		{
			snprintf(error, maxlength, "Could not find symbol: %s", pPatch->pSignature);
			dlclose(pServerSo);
			dlclose(pEngineSo);
			SDK_OnUnload();
			return false;
		}

		pPatch->pPatchAddress = FindPattern(pPatch->pAddress, pPatch->pPatchSignature, pPatch->pPatchPattern, 1024);
		if(!pPatch->pPatchAddress)
		{
			snprintf(error, maxlength, "Could not find patch signature for symbol: %s", pPatch->pSignature);
			dlclose(pServerSo);
			dlclose(pEngineSo);
			SDK_OnUnload();
			return false;
		}

		pPatch->pOriginal = (unsigned char *)malloc(PatchLen * sizeof(unsigned char));

		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_WRITE|SH_MEM_EXEC);
		for(int j = 0; j < PatchLen; j++)
		{
			pPatch->pOriginal[j] = *(unsigned char *)(pPatch->pPatchAddress + j);
			*(unsigned char *)(pPatch->pPatchAddress + j) = pPatch->pPatch[j];
		}
		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_EXEC);
	}

	dlclose(pServerSo);
	dlclose(pEngineSo);

	g_pOnRunThinkFunctions = forwards->CreateForward("OnRunThinkFunctions", ET_Ignore, 1, NULL, Param_Cell);
	g_pOnPrePlayerThinkFunctions = forwards->CreateForward("OnPrePlayerThinkFunctions", ET_Ignore, 0, NULL);
	g_pOnPostPlayerThinkFunctions = forwards->CreateForward("OnPostPlayerThinkFunctions", ET_Ignore, 0, NULL);
	g_pOnRunThinkFunctionsPost = forwards->CreateForward("OnRunThinkFunctionsPost", ET_Ignore, 1, NULL, Param_Cell);

	return true;
}

const sp_nativeinfo_t MyNatives[] =
{
	{ "BlockTriggerMoved", BlockTriggerMoved },
	{ "BlockTriggerTouchPlayers", BlockTriggerTouchPlayers },
	{ "BlockSolidMoved", BlockSolidMoved },
	{ "BlockSolidTouchPlayers", BlockSolidTouchPlayers },
	{ "FilterClientSolidTouch", FilterClientSolidTouch },
	{ NULL, NULL }
};

void PhysHooks::SDK_OnAllLoaded()
{
	sharesys->AddNatives(myself, MyNatives);
	sharesys->RegisterLibrary(myself, "PhysHooks");
}
void PhysHooks::SDK_OnUnload()
{
	if(g_pDetour_RunThinkFunctions != NULL)
	{
		g_pDetour_RunThinkFunctions->Destroy();
		g_pDetour_RunThinkFunctions = NULL;
	}

	if(g_pOnRunThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnRunThinkFunctions);
		g_pOnRunThinkFunctions = NULL;
	}

	if(g_pOnRunThinkFunctionsPost != NULL)
	{
		forwards->ReleaseForward(g_pOnRunThinkFunctionsPost);
		g_pOnRunThinkFunctionsPost = NULL;
	}

	if(g_pOnPrePlayerThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnPrePlayerThinkFunctions);
		g_pOnPrePlayerThinkFunctions = NULL;
	}

	if(g_pOnPostPlayerThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnPostPlayerThinkFunctions);
		g_pOnPostPlayerThinkFunctions = NULL;
	}

	if(g_SH_TriggerMoved)
		SH_REMOVE_HOOK_ID(g_SH_TriggerMoved);

	if(g_SH_TouchLinks)
		SH_REMOVE_HOOK_ID(g_SH_TouchLinks);

	SH_REMOVE_HOOK(IVEngineServer, TriggerMoved, engine, SH_STATIC(TriggerMoved), false);
	SH_REMOVE_HOOK(IVEngineServer, SolidMoved, engine, SH_STATIC(SolidMoved), false);

	gameconfs->CloseGameConfigFile(g_pGameConf);

	// Revert all applied patches
	for(size_t i = 0; i < sizeof(gs_Patches) / sizeof(*gs_Patches); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

		if(!pPatch->pOriginal)
			continue;

		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_WRITE|SH_MEM_EXEC);
		for(int j = 0; j < PatchLen; j++)
		{
			*(unsigned char *)(pPatch->pPatchAddress + j) = pPatch->pOriginal[j];
		}
		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_EXEC);

		free(pPatch->pOriginal);
		pPatch->pOriginal = NULL;
	}
}

bool PhysHooks::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	gpGlobals = ismm->GetCGlobals();
	return true;
}

uintptr_t FindPattern(uintptr_t BaseAddr, const unsigned char *pData, const char *pPattern, size_t MaxSize)
{
	unsigned char *pMemory;
	uintptr_t PatternLen = strlen(pPattern);

	pMemory = reinterpret_cast<unsigned char *>(BaseAddr);

	for(uintptr_t i = 0; i < MaxSize; i++)
	{
		uintptr_t Matches = 0;
		while(*(pMemory + i + Matches) == pData[Matches] || pPattern[Matches] != 'x')
		{
			Matches++;
			if(Matches == PatternLen)
				return (uintptr_t)(pMemory + i);
		}
	}

	return 0x00;
}
