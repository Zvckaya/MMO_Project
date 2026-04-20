#pragma once
#include <stdint.h>

using  PlayerID = uint64_t;

class Player
{
public:
	Player(PlayerID playerID):_playerID(playerID),posX(0),poxY(0)
	{
	}


	~Player()
	{

	}

	int posX;
	int poxY;

private:
	uint64_t _playerID;
};