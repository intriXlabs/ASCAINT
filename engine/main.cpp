#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>

class engine {
public:
    int rows = 0;
    int cols = 0;

private:
    char* display = nullptr;
    std::string defaultColor = "\033[38;2;255;255;255m"; // Reset to default color

private:
    void updateSize() {
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
            rows = ws.ws_row;
            cols = ws.ws_col;
        }
    }

    void makeDisplay() {
        display = new char[(rows * cols) + rows]; // +rows for newline characters
        for (int i = 0; i < rows * cols; ++i) {
            display[i] = ' ';
        }
    }

    int makeAndPrintBuffer() {
        std::string buffer;
        buffer.reserve((rows * cols) + (rows * 10) + 30);

        buffer += "\033[H";
        buffer += defaultColor;
        

        for (int r = 0; r < rows; ++r) {
            buffer += "\033[";
            buffer += std::to_string(r + 1);
            buffer += ";1H";

            for (int c = 0; c < cols; ++c) {
                buffer += display[r * cols + c];
            }
        }

        size_t totalWritten = 0;

        while (totalWritten < buffer.size()) {
            ssize_t written = write(
                STDOUT_FILENO,
                buffer.data() + totalWritten,
                buffer.size() - totalWritten
            );

            if (written < 0) {
                if (errno == EINTR)
                    continue;

                return -1;
            }

            if (written == 0)
                return -1;

            totalWritten += static_cast<size_t>(written);
        }

        return static_cast<int>(totalWritten);
    }

public:
    engine() {
        updateSize();
        makeDisplay();          // <-- was missing: allocate the buffer
    }

    ~engine() {
        delete[] display;       // <-- free the buffer
    }

    engine(const engine&) = delete;             // prevent shallow copy of pointer
    engine& operator=(const engine&) = delete;

    void setChar(int row, int col, char c) {
        if (row < 0 || row >= rows || col < 0 || col >= cols) {
            return; // Out of bounds
        }
        display[row * cols + col] = c;
    }

    char getChar(int row, int col) const {
        if (row < 0 || row >= rows || col < 0 || col >= cols) {
            return '\0'; // Out of bounds
        }
        return display[row * cols + col];
    }

    void setDefaultColor(const std::string hexColor) {
        if (hexColor.size() == 7 && hexColor[0] == '#') {
            int r = std::stoi(hexColor.substr(1, 2), nullptr, 16);
            int g = std::stoi(hexColor.substr(3, 2), nullptr, 16);
            int b = std::stoi(hexColor.substr(5, 2), nullptr, 16);
            defaultColor = "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
        }
    }

    void render() {
        makeAndPrintBuffer();
    }

    std::string getBuffer() const {
        return std::string(display, rows * cols);
    }
};