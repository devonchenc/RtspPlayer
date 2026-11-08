#include "RtspPlayer.h"

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout << "Usage: RtspPlayer <RTSP URL>\n";
        return 1;
    }

    RtspPlayer player;

    if (!player.Open(argv[1]))
    {
        std::cerr << "Failed to open RTSP stream\n";
        return 1;
    }

    player.Play();

    return 0;
}