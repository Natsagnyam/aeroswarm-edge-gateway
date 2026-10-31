#pragma once
#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

class GatewayHardware {
private:
    volatile uint32_t* gpio1_datain = nullptr;

public:
    bool initialize() {
        int fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (fd < 0) {
            std::perror("[Hardware] Failed to open /dev/mem");
            return false;
        }

        size_t page_size = sysconf(_SC_PAGE_SIZE);

        // Map Control Module for P9_12 pinmux
        void* ctrl_base = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x44E10000);
        if (ctrl_base != MAP_FAILED) {
            volatile uint32_t* pin_reg = (volatile uint32_t*)((char*)ctrl_base + 0x878);
            *pin_reg = 0x37; // Mode 7 + RXACTIVE
            munmap(ctrl_base, page_size);
        }

        // Map GPIO1 module
        void* gpio1_base = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x4804C000);
        close(fd);

        if (gpio1_base == MAP_FAILED) {
            std::perror("[Hardware] GPIO1 mmap failed");
            return false;
        }

        // Set GPIO1_28 as input
        volatile uint32_t* gpio1_oe = (volatile uint32_t*)((char*)gpio1_base + 0x134);
        *gpio1_oe |= (1 << 28);

        gpio1_datain = (volatile uint32_t*)((char*)gpio1_base + 0x138);
        std::cout << "[Hardware] MMIO initialized successfully.\n";
        return true;
    }

    bool readPushButton() {
        if (!gpio1_datain) return false;
        return ((*gpio1_datain & (1 << 28)) == 0); // Active low
    }

    void setLedState(int led_num, bool state) {
        // BBB User LEDs can be controlled via standard Linux sysfs or GPIO registers
        std::string path = "/sys/class/leds/beaglebone:green:usr" + std::to_string(led_num) + "/brightness";
        std::ofstream led(path);
        if (led.is_open()) {
            led << (state ? 1 : 0);
            led.close();
        }
    }
};
