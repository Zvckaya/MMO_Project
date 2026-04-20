
#include "ServerConfig.h"
#include "GameServer.h"
#include "Logger.h"
#include <iostream>

GameServer gameServer;



int main()
{
	Logger::instance().setDirectory(L"Logs");
	Logger::instance().setLevel(Logger::Level::DEBUG);

	if (!gameServer.Start(std::nullopt, PORT, 16, 8, false, 20000))
	{
		return 1;
	}

	char input;
	while (std::cin.get(input))
	{
		if (input == 'q' || input == 'Q')
			break;
	}

	gameServer.Stop();
	return 0;
}

