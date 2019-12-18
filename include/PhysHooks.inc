#if defined _physhooks_included
 #endinput
#endif
#define _physhooks_included

forward void OnRunThinkFunctions(bool simulating);
forward void OnPrePlayerThinkFunctions();
forward void OnPostPlayerThinkFunctions();
forward void OnRunThinkFunctionsPost(bool simulating);

// Block TriggerMoved from being called at all for an entity by setting the bit to 1.
native void BlockTriggerMoved(int map[2048 / 32], bool set);

// Block triggers TriggerMoved from touching any client by setting the bit to 1 for the entity index.
native void BlockTriggerTouchPlayers(int map[2048 / 32], bool set);

// Block SolidMoved from being called at all for an entity by setting the bit to 1.
native void BlockSolidMoved(int map[2048 / 32], bool set);

// Block solids SolidMoved from touching any client by setting the bit to 1 for the entity index.
native void BlockSolidTouchPlayers(int map[2048 / 32], bool set);

// Block clients SolidMoved from touching an entity by setting the bit to 1 in the clients map.
native void FilterClientSolidTouch(int map[((MAXPLAYERS + 1) * 2048) / 32], bool set);


public Extension __ext_PhysHooks =
{
	name = "PhysHooks",
	file = "PhysHooks.ext",
#if defined AUTOLOAD_EXTENSIONS
	autoload = 1,
#else
	autoload = 0,
#endif
#if defined REQUIRE_EXTENSIONS
	required = 1,
#else
	required = 0,
#endif
};

#if !defined REQUIRE_EXTENSIONS
public __ext_PhysHooks_SetNTVOptional()
{
}
#endif
