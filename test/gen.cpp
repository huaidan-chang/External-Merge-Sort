#include <iostream>
#include <fstream>
#include <cstdlib>

#define REC_SIZE 1000
#define REC_NUM 100

int main()
{
    std::ofstream outputFile("input.txt");
    char rec_buf[REC_SIZE];
    if (!outputFile.is_open())
    {
        std::cerr << "Error opening input file." << std::endl;
        return 1;
    }

    for (int row = 0; row < REC_NUM; ++row)
    {
        // Part 1: 332 bytes
        for (int i = 0; i < 332; ++i)
        {
            rec_buf[i] = std::rand() % 2 == 0 ? 'a' + std::rand() % 26 : '0' + std::rand() % 10;
        }

        rec_buf[332] = ' ';

        // Part 2: 332 bytes
        for (int i = 333; i < 665; ++i)
        {
            rec_buf[i] = std::rand() % 2 == 0 ? 'a' + std::rand() % 26 : '0' + std::rand() % 10;
        }

        rec_buf[665] = ' ';

        // Part 3: 332 bytes
        for (int i = 666; i < 998; ++i)
        {
            rec_buf[i] = std::rand() % 2 == 0 ? 'a' + std::rand() % 26 : '0' + std::rand() % 10;
        }

        /* add 2 bytes of "break" data */
        rec_buf[998] = '\r'; /* nice for Windows */
        rec_buf[999] = '\n';

        outputFile.write(rec_buf, REC_SIZE);
    }

    outputFile.close();

    std::cout << "Data generation complete." << std::endl;

    return 0;
}
