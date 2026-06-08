#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "NUC1xx.h"
#include "GPIO.h"
#include "LCD.h"
#include "Scankey.h"
#include "2DGraphics.h"
#include "SYS.h"
#include "Seven_Segment.h"
#include "I2C.h"

// Timer operation modes
#define ONESHOT     0
#define PERIODIC    1
#define TOGGLE      2
#define CONTINUOUS  3

// Game constants
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define LANE_WIDTH 42
#define TILE_HEIGHT 8
#define HIT_ZONE_Y 56
#define SPAWN_Y 0
#define MAX_TILES 10
#define PERFECT_ZONE 4

// EEPROM addresses for leaderboard (3 difficulties x 3 scores each)
// Easy difficulty scores
#define EASY_SCORE1_ADDR 0x0000
#define EASY_SCORE2_ADDR 0x0004
#define EASY_SCORE3_ADDR 0x0008

// Medium difficulty scores  
#define MEDIUM_SCORE1_ADDR 0x0010
#define MEDIUM_SCORE2_ADDR 0x0014
#define MEDIUM_SCORE3_ADDR 0x0018

// Hard difficulty scores
#define HARD_SCORE1_ADDR 0x0020
#define HARD_SCORE2_ADDR 0x0024
#define HARD_SCORE3_ADDR 0x0028

// LED definitions - GPC12 to GPC15 from left to right
#define LEFT_LED     12  // GPC.12 - Leftmost LED
#define LEFT_CENTER  13  // GPC.13
#define RIGHT_CENTER 14  // GPC.14
#define RIGHT_LED    15  // GPC.15 - Rightmost LED

// EEPROM timeout for I2C operations
#define I2C_TIMEOUT 100000

// Game states
typedef enum {
    GAME_MAIN_MENU,
    GAME_DIFFICULTY_MENU,
    GAME_LEADERBOARD,
    GAME_PLAYING,
    GAME_OVER
} GameState;

// Tile structure
typedef struct {
    uint8_t lane;      // 0, 1, or 2
    int16_t y;         // Y position
    uint8_t active;    // Is tile active
    uint8_t hit;       // Has been hit
} Tile;

// Game variables
GameState gameState = GAME_MAIN_MENU;
Tile tiles[MAX_TILES];
volatile uint16_t score = 0;
uint8_t lives = 3;
uint16_t gameSpeed = 3; // Lower = faster
uint16_t speedCounter = 0;
uint16_t spawnCounter = 0;
uint8_t spawnRate = 30; // Frames between spawns
uint8_t lastKey = 0;
static uint8_t display_digit = 0;

// Menu variables
uint8_t currentMenuOption = 1; // 1, 2, or 3
uint8_t FALL_SPEED = 5; // Default medium difficulty
uint8_t CURRENT_DIFFICULTY = 1; // 0=Easy, 1=Medium, 2=Hard
uint8_t inDifficultyMenu = 0; // Flag to control when ADC affects difficulty
uint32_t gameOverCounter = 0; // Counter for game over screen
uint8_t leaderboardPage = 0; // 0=Easy, 1=Medium, 2=Hard for viewing scores
uint8_t lastLeaderboardPage = 255; // Track last page to prevent constant redraw

// EEPROM status flag
uint8_t eeprom_available = 0; // 0 = not available, 1 = available

// Function prototypes
void InitGame(void);
void UpdateGame(void);
void DrawGame(void);
void SpawnTile(void);
void MoveTiles(void);
void CheckCollisions(uint8_t key);
void DrawTile(uint8_t lane, int16_t y);
void DrawLanes(void);
void DrawHUD(void);
void DrawMainMenu(void);
void DrawDifficultyMenu(void);
void DrawLeaderboard(void);
void DrawGameOver(void);
void ResetGameVariables(void);
uint8_t GetLaneX(uint8_t lane);
void InitTIMER2(void);
void TMR2_IRQHandler(void);
void seg_display_score(uint16_t value);
void InitADC(void);
int GetADCValue(void);
void UpdateLeaderboard(uint32_t newScore);
uint32_t ReadScoreFromEEPROM(int position, uint8_t difficulty);
void WriteScoreToEEPROM(int position, uint32_t score, uint8_t difficulty);
uint8_t Write_24LC64_Safe(uint32_t address, uint8_t data);
uint8_t Read_24LC64_Safe(uint32_t address, uint8_t *data);
void delay_ms(uint32_t ms);
void InitI2C1_EEPROM(void);
void TestEEPROM(void);
void ResetAllScores(void);
void ManualResetEEPROM(void);

// Delay function for proper EEPROM timing
void delay_ms(uint32_t ms)
{
    uint32_t i, j;
    for(i = 0; i < ms; i++)
        for(j = 0; j < 5000; j++);  // Adjust based on your clock speed
}

// Safe EEPROM Write Function with timeout
uint8_t Write_24LC64_Safe(uint32_t address, uint8_t data)
{
    uint32_t i;
    uint32_t timeout;
    
    if (!eeprom_available) return 0;  // Don't try if EEPROM not available
    
    // Make sure I2C1 pins are configured
    DrvGPIO_InitFunction(E_FUNC_I2C1);
    
    SystemCoreClock = DrvSYS_GetHCLKFreq(); 
    DrvI2C_Open(I2C_PORT1, 50000);
    
    // Small delay after opening I2C
    for(i=0;i<1000;i++);
    
    // Start condition with timeout
    DrvI2C_Ctrl(I2C_PORT1, 1, 0, 0, 0);
    timeout = 0;
    while (I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;  // Mark EEPROM as unavailable
            return 0;
        }
    }
    
    // Send device address
    I2C1->I2CDAT = 0xA0;
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 0);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Send high address byte
    I2C1->I2CDAT = (address>>8)&0xFF;    
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 1);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Send low address byte
    I2C1->I2CDAT = address&0xFF;        
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 1);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Send data
    I2C1->I2CDAT = data;
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 1);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Stop condition
    DrvI2C_Ctrl(I2C_PORT1, 0, 1, 1, 0);
    timeout = 0;
    while(I2C1->I2CON.STO) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    DrvI2C_Close(I2C_PORT1);
    
    // CRITICAL: 24LC64 requires 5ms write cycle time
    delay_ms(10);  // Use 10ms to be safe
    
    return 1;  // Success
}

// Safe EEPROM Read Function with timeout
uint8_t Read_24LC64_Safe(uint32_t address, uint8_t *data)
{
    uint32_t i;
    uint32_t timeout;
    
    if (!eeprom_available) return 0;  // Don't try if EEPROM not available
    
    // Make sure I2C1 pins are configured
    DrvGPIO_InitFunction(E_FUNC_I2C1);
    
    SystemCoreClock = DrvSYS_GetHCLKFreq(); 
    DrvI2C_Open(I2C_PORT1, 50000);
    
    // Small delay after opening I2C
    for(i=0;i<1000;i++);
    
    // Start condition with timeout
    DrvI2C_Ctrl(I2C_PORT1, 1, 0, 1, 0);
    timeout = 0;
    while (I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Send device address for write
    I2C1->I2CDAT = 0xA0;
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 0);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Send high address byte
    I2C1->I2CDAT = (address>>8)&0xFF;    
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 1);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Send low address byte
    I2C1->I2CDAT = address&0xFF;        
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 0);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Restart condition
    DrvI2C_Ctrl(I2C_PORT1, 1, 0, 1, 0);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Send device address for read
    I2C1->I2CDAT = 0xA1;
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 1);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Prepare to receive data
    I2C1->I2CDAT = 0xFF;
    DrvI2C_Ctrl(I2C_PORT1, 0, 0, 1, 0);
    timeout = 0;
    while(I2C1->I2CON.SI == 0) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    // Read data
    *data = I2C1->I2CDAT;
    
    // Stop condition
    DrvI2C_Ctrl(I2C_PORT1, 0, 1, 1, 0);
    timeout = 0;
    while(I2C1->I2CON.STO) {
        if (++timeout > I2C_TIMEOUT) {
            DrvI2C_Close(I2C_PORT1);
            eeprom_available = 0;
            return 0;
        }
    }
    
    DrvI2C_Close(I2C_PORT1);
    
    // Small delay after read operation
    delay_ms(1);
    
    return 1;  // Success
}

// Initialize ADC
void InitADC(void)
{
    GPIOA->OFFD |= 0x00800000;
    SYS->GPAMFP.ADC7_SS21_AD6 = 1;
    SYSCLK->CLKSEL1.ADC_S = 2;
    SYSCLK->CLKDIV.ADC_N = 1;
    SYSCLK->APBCLK.ADC_EN = 1;
    ADC->ADCR.ADEN = 1;
    ADC->ADCR.DIFFEN = 0;
    ADC->ADCR.ADMD = 0;
    ADC->ADCHER.CHEN = 0x80;
    ADC->ADSR.ADF = 1;
}

// Get ADC value
int GetADCValue(void)
{
    ADC->ADCR.ADST = 1;
    while(ADC->ADSR.ADF == 0);
    ADC->ADSR.ADF = 1;
    int adc_raw = ADC->ADDR[7].RSLT & 0x0FFC;
    return (adc_raw * 3) / 4095;
}

// Initialize I2C1 for EEPROM
void InitI2C1_EEPROM(void)
{
    // CRITICAL: Configure GPIO pins for I2C1 function
    // PD4 = I2C1_SCL, PD5 = I2C1_SDA
    DrvGPIO_InitFunction(E_FUNC_I2C1);
    
    // Enable I2C1 clock
    SYSCLK->APBCLK.I2C1_EN = 1;
    
    // Reset I2C1
    SYS->IPRSTC2.I2C1_RST = 1;
    SYS->IPRSTC2.I2C1_RST = 0;
    
    // Small delay after reset
    delay_ms(1);
}

// Manual EEPROM reset function (can be called anywhere)
void ManualResetEEPROM(void)
{
    uint16_t addr;
    char statusText[20];
    
    if (!eeprom_available) return;
    
    // Clear entire score area (addresses 0x0000 to 0x002B)
    // This covers all 9 scores (3 difficulties x 3 positions x 4 bytes each)
    for(addr = 0x0000; addr <= 0x002B; addr++) {
        Write_24LC64_Safe(addr, 0x00);
        
        // Show progress every 4 bytes
        if(addr % 4 == 0) {
            clear_LCD();
            printS_5x7(20, 25, "Clearing EEPROM");
            sprintf(statusText, "Address: 0x%04X", addr);
            printS_5x7(20, 35, statusText);
        }
    }
    
    clear_LCD();
    printS_5x7(25, 30, "EEPROM Cleared!");
    delay_ms(1000);
}

// Reset all scores in EEPROM
void ResetAllScores(void)
{
    int i, j;
    
    if (!eeprom_available) return;
    
    // Show reset message
    clear_LCD();
    printS_5x7(25, 20, "RESETTING SCORES");
    printS_5x7(30, 30, "Please wait...");
    
    // Reset all scores for all difficulties (3 difficulties x 3 scores each)
    for(i = 0; i < 3; i++) {  // For each difficulty (0=Easy, 1=Medium, 2=Hard)
        for(j = 1; j <= 3; j++) {  // For each position (1st, 2nd, 3rd)
            WriteScoreToEEPROM(j, 0, i);
            delay_ms(5);  // Small delay between writes
        }
        
        // Show progress
        clear_LCD();
        printS_5x7(25, 20, "RESETTING SCORES");
        if(i == 0) printS_5x7(35, 30, "Easy...done");
        else if(i == 1) printS_5x7(30, 30, "Medium...done");
        else printS_5x7(35, 30, "Hard...done");
        delay_ms(200);
    }
    
    // Show completion message
    clear_LCD();
    printS_5x7(25, 25, "SCORES RESET!");
    printS_5x7(15, 35, "All scores cleared");
    delay_ms(1000);
    clear_LCD();
}

// Test EEPROM availability
void TestEEPROM(void)
{
    uint8_t test_data;
    uint8_t write_test = 0x55;  // Test pattern
    uint32_t test_score;
    int i, j;
    
    // Show testing message on LCD
    clear_LCD();
    printS_5x7(20, 20, "Initializing...");
    printS_5x7(15, 35, "Testing Memory");
    
    // Initialize I2C1 for EEPROM
    InitI2C1_EEPROM();
    
    // Assume EEPROM is available initially
    eeprom_available = 1;
    
    // Try to write and read back a test byte
    if (Write_24LC64_Safe(0x0100, write_test)) {  // Use address 0x0100 for test
        delay_ms(10);  // Wait for write to complete
        if (Read_24LC64_Safe(0x0100, &test_data)) {
            if (test_data == write_test) {
                // EEPROM is working properly
                eeprom_available = 1;
                
                // Initialize all scores if they're uninitialized
                for(i = 0; i < 3; i++) {  // 3 difficulties
                    for(j = 1; j <= 3; j++) {  // 3 scores each
                        test_score = ReadScoreFromEEPROM(j, i);
                        if(test_score == 0xFFFFFFFF) {
                            WriteScoreToEEPROM(j, 0, i);
                            delay_ms(5);
                        }
                    }
                }
            } else {
                // Read back didn't match
                eeprom_available = 0;
            }
        } else {
            // Read failed
            eeprom_available = 0;
        }
    } else {
        // Write failed
        eeprom_available = 0;
    }
    
    // Clear the initialization message
    clear_LCD();
}

// Read score from EEPROM with fallback
uint32_t ReadScoreFromEEPROM(int position, uint8_t difficulty)
{
    uint32_t score = 0;
    uint16_t addr;
    uint8_t byte_val;
    
    // If EEPROM not available, return 0
    if (!eeprom_available) return 0;
    
    // Calculate address based on difficulty and position
    switch(difficulty) {
        case 0: // Easy
            switch(position) {
                case 1: addr = EASY_SCORE1_ADDR; break;
                case 2: addr = EASY_SCORE2_ADDR; break;
                case 3: addr = EASY_SCORE3_ADDR; break;
                default: return 0;
            }
            break;
        case 1: // Medium
            switch(position) {
                case 1: addr = MEDIUM_SCORE1_ADDR; break;
                case 2: addr = MEDIUM_SCORE2_ADDR; break;
                case 3: addr = MEDIUM_SCORE3_ADDR; break;
                default: return 0;
            }
            break;
        case 2: // Hard
            switch(position) {
                case 1: addr = HARD_SCORE1_ADDR; break;
                case 2: addr = HARD_SCORE2_ADDR; break;
                case 3: addr = HARD_SCORE3_ADDR; break;
                default: return 0;
            }
            break;
        default:
            return 0;
    }
    
    // Read each byte with error checking
    if (!Read_24LC64_Safe(addr, &byte_val)) return 0;
    score = byte_val;
    
    if (!Read_24LC64_Safe(addr + 1, &byte_val)) return 0;
    score |= ((uint32_t)byte_val) << 8;
    
    if (!Read_24LC64_Safe(addr + 2, &byte_val)) return 0;
    score |= ((uint32_t)byte_val) << 16;
    
    if (!Read_24LC64_Safe(addr + 3, &byte_val)) return 0;
    score |= ((uint32_t)byte_val) << 24;
    
    // Check for uninitialized EEPROM (all 0xFF)
    if(score == 0xFFFFFFFF || score > 99999) {
        score = 0;
    }
    
    return score;
}

// Write score to EEPROM with error checking
void WriteScoreToEEPROM(int position, uint32_t score, uint8_t difficulty)
{
    uint16_t addr;
    
    // If EEPROM not available, return
    if (!eeprom_available) return;
    
    // Calculate address based on difficulty and position
    switch(difficulty) {
        case 0: // Easy
            switch(position) {
                case 1: addr = EASY_SCORE1_ADDR; break;
                case 2: addr = EASY_SCORE2_ADDR; break;
                case 3: addr = EASY_SCORE3_ADDR; break;
                default: return;
            }
            break;
        case 1: // Medium
            switch(position) {
                case 1: addr = MEDIUM_SCORE1_ADDR; break;
                case 2: addr = MEDIUM_SCORE2_ADDR; break;
                case 3: addr = MEDIUM_SCORE3_ADDR; break;
                default: return;
            }
            break;
        case 2: // Hard
            switch(position) {
                case 1: addr = HARD_SCORE1_ADDR; break;
                case 2: addr = HARD_SCORE2_ADDR; break;
                case 3: addr = HARD_SCORE3_ADDR; break;
                default: return;
            }
            break;
        default:
            return;
    }
    
    // Validate score before writing
    if(score > 99999) score = 99999;
    
    // Write each byte with error checking
    Write_24LC64_Safe(addr, score & 0xFF);
    Write_24LC64_Safe(addr + 1, (score >> 8) & 0xFF);
    Write_24LC64_Safe(addr + 2, (score >> 16) & 0xFF);
    Write_24LC64_Safe(addr + 3, (score >> 24) & 0xFF);
}

// Update leaderboard with error handling
void UpdateLeaderboard(uint32_t newScore)
{
    uint32_t scores[3];
    int i, j;
    
    // Validate input
    if(newScore == 0 || newScore > 99999) return;
    
    // If EEPROM not available, skip
    if (!eeprom_available) return;
    
    // Display saving message
    clear_LCD();
    printS_5x7(35, 28, "SAVING...");
    
    // Add delay before starting EEPROM operations
    delay_ms(5);
    
    // Read existing scores for current difficulty with delays
    for(i = 0; i < 3; i++) {
        scores[i] = ReadScoreFromEEPROM(i + 1, CURRENT_DIFFICULTY);
        delay_ms(2);  // Small delay between reads
    }
    
    // Check if new score qualifies for leaderboard
    if(newScore <= scores[2] && scores[2] != 0) return;  // Not a high score
    
    // Insert new score in the correct position
    for(i = 0; i < 3; i++) {
        if(newScore > scores[i]) {
            // Shift scores down
            for(j = 2; j > i; j--) {
                scores[j] = scores[j-1];
            }
            scores[i] = newScore;
            break;
        }
    }
    
    // Write updated scores back to EEPROM for current difficulty
    for(i = 0; i < 3; i++) {
        WriteScoreToEEPROM(i + 1, scores[i], CURRENT_DIFFICULTY);
        delay_ms(2);  // Small delay between write operations
    }
    
    // Final delay to ensure all writes complete
    delay_ms(10);
}

// Initialize Timer2
void InitTIMER2(void)
{
    SYSCLK->CLKSEL1.TMR2_S = 0;
    SYSCLK->APBCLK.TMR2_EN = 1;
    TIMER2->TCSR.MODE = PERIODIC;
    TIMER2->TCSR.PRESCALE = 255;
    TIMER2->TCMPR = 75;
    TIMER2->TCSR.IE = 1;
    TIMER2->TISR.TIF = 1;
    NVIC_EnableIRQ(TMR2_IRQn);
    TIMER2->TCSR.CRST = 1;
    TIMER2->TCSR.CEN = 1;
}

// Timer2 interrupt handler
void TMR2_IRQHandler(void)
{
    seg_display_score(score);
    TIMER2->TISR.TIF = 1;
}

// Display score on 7-segment
void seg_display_score(uint16_t value)
{
    int8_t digit;
    
    if(value > 9999) value = 9999;
    
    close_seven_segment();
    
    switch(display_digit) {
        case 0:
            digit = value / 1000;
            show_seven_segment(3, digit);
            break;
        case 1:
            digit = (value / 100) % 10;
            show_seven_segment(2, digit);
            break;
        case 2:
            digit = (value / 10) % 10;
            show_seven_segment(1, digit);
            break;
        case 3:
            digit = value % 10;
            show_seven_segment(0, digit);
            break;
    }
    
    display_digit = (display_digit + 1) % 4;
}

// Draw main menu
void DrawMainMenu(void) {
    clear_LCD();
    
    // Turn off all LEDs first
    DrvGPIO_SetBit(E_GPC, LEFT_LED);
    DrvGPIO_SetBit(E_GPC, LEFT_CENTER);
    DrvGPIO_SetBit(E_GPC, RIGHT_CENTER);
    DrvGPIO_SetBit(E_GPC, RIGHT_LED);
    
    printS_5x7(33, 2, "RHYTHM TILES");
    
    if (!eeprom_available) {
        printS_5x7(28, 10, "(No Memory)");
    }
 
    if(currentMenuOption == 1)
        printS_5x7(10, 17, ">1.START");
    else
        printS_5x7(10, 17, " 1.START");
        
    if(currentMenuOption == 2)
        printS_5x7(10, 30, ">2.DIFFICULTY");
    else
        printS_5x7(10, 30, " 2.DIFFICULTY");
        
    if(currentMenuOption == 3)
        printS_5x7(10, 47, ">3.SCORES");
    else
        printS_5x7(10, 47, " 3.SCORES");
    
    printS_5x7(11, 56, "(1:Up 2:Select 3:Down)");
}

// Draw difficulty menu
void DrawDifficultyMenu(void) {
    char difficultyText[15];
    clear_LCD();
    printS_5x7(39, 2, "DIFFICULTY");
    printS_5x7(45, 17, "Use VR1:");
    
    if(inDifficultyMenu) {
        int adcValue = GetADCValue();
        
        if(adcValue == 0) {
            FALL_SPEED = 2;
            CURRENT_DIFFICULTY = 0;  // Easy
            strcpy(difficultyText, " (EASY) ");
            DrvGPIO_SetBit(E_GPC, LEFT_CENTER);
            DrvGPIO_SetBit(E_GPC, RIGHT_CENTER);
            DrvGPIO_ClrBit(E_GPC, LEFT_LED);
            DrvGPIO_SetBit(E_GPC, RIGHT_LED);
        }
        else if(adcValue == 1) {
            FALL_SPEED = 5;
            CURRENT_DIFFICULTY = 1;  // Medium
            strcpy(difficultyText, "(MEDIUM)");
            DrvGPIO_SetBit(E_GPC, RIGHT_CENTER);
            DrvGPIO_ClrBit(E_GPC, LEFT_CENTER);
            DrvGPIO_ClrBit(E_GPC, LEFT_LED);
            DrvGPIO_SetBit(E_GPC, RIGHT_LED);
        }
        else {
            FALL_SPEED = 8;
            CURRENT_DIFFICULTY = 2;  // Hard
            strcpy(difficultyText, " (HARD) ");
            DrvGPIO_ClrBit(E_GPC, RIGHT_CENTER);
            DrvGPIO_ClrBit(E_GPC, LEFT_CENTER);
            DrvGPIO_ClrBit(E_GPC, LEFT_LED);
            DrvGPIO_SetBit(E_GPC, RIGHT_LED);
        }
    } else {
        if(CURRENT_DIFFICULTY == 0) strcpy(difficultyText, "EASY");
        else if(CURRENT_DIFFICULTY == 1) strcpy(difficultyText, "MEDIUM");
        else strcpy(difficultyText, "HARD");
    }
    
    printS_5x7(47, 32, difficultyText);
    printS_5x7(20, 50, "Press 3 to confirm");
}

// Draw leaderboard
void DrawLeaderboard(void) {
    char scoreText[20];
    uint32_t scoreValue;
    int i;
    char* difficultyName;
    int adcValue;
    uint8_t needsRedraw = 0;
    
    // Read ADC and determine page
    adcValue = GetADCValue();
    uint8_t newPage;
    
    if(adcValue == 0) {
        newPage = 0;  // Easy
    }
    else if(adcValue == 1) {
        newPage = 1;  // Medium
    }
    else {
        newPage = 2;  // Hard
    }
    
    // Only update if page changed
    if(newPage != leaderboardPage || lastLeaderboardPage == 255) {
        leaderboardPage = newPage;
        needsRedraw = 1;
    }
    
    // Only redraw if needed (prevents jittering)
    if(!needsRedraw && lastLeaderboardPage != 255) {
        return;
    }
    
    lastLeaderboardPage = leaderboardPage;
    
    // Clear and redraw everything
    clear_LCD();
    
    // Set LEDs based on difficulty
    if(leaderboardPage == 0) {
        // Easy - 2 center LEDs
        DrvGPIO_SetBit(E_GPC, LEFT_CENTER);
        DrvGPIO_SetBit(E_GPC, RIGHT_CENTER);
        DrvGPIO_ClrBit(E_GPC, LEFT_LED);
        DrvGPIO_SetBit(E_GPC, RIGHT_LED);
    }
    else if(leaderboardPage == 1) {
        // Medium - 1 center LED
        DrvGPIO_SetBit(E_GPC, RIGHT_CENTER);
        DrvGPIO_ClrBit(E_GPC, LEFT_CENTER);
        DrvGPIO_ClrBit(E_GPC, LEFT_LED);
        DrvGPIO_SetBit(E_GPC, RIGHT_LED);
    }
    else {
        // Hard - minimal LEDs
        DrvGPIO_ClrBit(E_GPC, RIGHT_CENTER);
        DrvGPIO_ClrBit(E_GPC, LEFT_CENTER);
        DrvGPIO_ClrBit(E_GPC, LEFT_LED);
        DrvGPIO_SetBit(E_GPC, RIGHT_LED);
    }
    
    if (eeprom_available) {
        // Get difficulty name
        switch(leaderboardPage) {
            case 0: difficultyName = "EASY"; break;
            case 1: difficultyName = "MEDIUM"; break;
            case 2: difficultyName = "HARD"; break;
            default: difficultyName = "EASY"; break;
        }
        
        // Simple title
        sprintf(scoreText, "(%s) HIGH SCORES", difficultyName);
        printS_5x7(15, 10, scoreText);
        
        // Display all 3 scores with proper spacing
        for(i = 1; i <= 3; i++) {
            scoreValue = ReadScoreFromEEPROM(i, leaderboardPage);
            
            if(scoreValue > 0) {
                sprintf(scoreText, "%d.  %4lu", i, scoreValue);
            } else {
                sprintf(scoreText, "%d.  ----", i);
            }
            // Adjusted Y positioning to show all 3 scores properly
            printS_5x7(40, 16 + (i * 9), scoreText);
        }
        
        // Instructions at bottom with clear spacing
        printS_5x7(10, 56, "VR1:Change");
        printS_5x7(80, 56, "3:Back");  // Moved slightly right to avoid overlap
        
    } else {
        printS_5x7(20, 25, "Memory not available");
        printS_5x7(20, 50, "Press 3 to return");
    }
}

// Reset game variables
void ResetGameVariables(void) {
    int i;
    for(i = 0; i < MAX_TILES; i++) {
        tiles[i].active = 0;
        tiles[i].hit = 0;
        tiles[i].y = 0;
        tiles[i].lane = 0;
    }
    
    score = 0;
    lives = 3;
    speedCounter = 0;
    spawnCounter = 0;
    spawnRate = 30;
    gameOverCounter = 0;
    
    // Set default difficulty to Medium if not already set
    if(FALL_SPEED <= 2) CURRENT_DIFFICULTY = 0;     // Easy
    else if(FALL_SPEED <= 5) CURRENT_DIFFICULTY = 1; // Medium
    else CURRENT_DIFFICULTY = 2;                     // Hard
}

// Initialize game
void InitGame(void) {
    ResetGameVariables();
    gameSpeed = FALL_SPEED;
    gameState = GAME_PLAYING;
    SpawnTile();
    
    DrvGPIO_SetBit(E_GPC, LEFT_LED);
    DrvGPIO_SetBit(E_GPC, LEFT_CENTER);
    DrvGPIO_SetBit(E_GPC, RIGHT_CENTER);
    DrvGPIO_SetBit(E_GPC, RIGHT_LED);
}

// Get X position for lane
uint8_t GetLaneX(uint8_t lane) {
    return 1 + (lane * LANE_WIDTH);
}

// Draw tile
void DrawTile(uint8_t lane, int16_t y) {
    uint8_t x = GetLaneX(lane);
    RectangleFill(x + 5, y, x + LANE_WIDTH - 6, y + TILE_HEIGHT - 1, 1, 0);
}

// Draw lanes
void DrawLanes(void) {
    int i;
    
    LineBresenham(LANE_WIDTH, 0, LANE_WIDTH, SCREEN_HEIGHT-1, 1, 0);
    LineBresenham(LANE_WIDTH * 2, 0, LANE_WIDTH * 2, SCREEN_HEIGHT-1, 1, 0);
    
    for(i = 0; i < 3; i++) {
        uint8_t x = GetLaneX(i);
        RectangleDraw(x + 2, HIT_ZONE_Y - 2, x + LANE_WIDTH - 3, HIT_ZONE_Y + 2, 1, 0);
    }
    
    printC_5x7(GetLaneX(0) + 18, 58, '1');
    printC_5x7(GetLaneX(1) + 18, 58, '2');
    printC_5x7(GetLaneX(2) + 18, 58, '3');
}

// Draw HUD
void DrawHUD(void) {
    char buffer[10];
    char* diffText;
    
    // Show lives
    sprintf(buffer, "Lives:%d", lives);
    printS_5x7(89, 2, buffer);
    
    // Show difficulty
    switch(CURRENT_DIFFICULTY) {
        case 0: diffText = " EASY"; break;
        case 1: diffText = "MEDIUM"; break;
        case 2: diffText = " HARD"; break;
        default: diffText = "?"; break;
    }
    printS_5x7(5, 2, diffText);
}

// Spawn tile
void SpawnTile(void) {
    int i;
    for(i = 0; i < MAX_TILES; i++) {
        if(!tiles[i].active) {
            tiles[i].lane = rand() % 3;
            tiles[i].y = SPAWN_Y;
            tiles[i].active = 1;
            tiles[i].hit = 0;
            break;
        }
    }
}

// Move tiles
void MoveTiles(void) {
    int i;
    for(i = 0; i < MAX_TILES; i++) {
        if(tiles[i].active && !tiles[i].hit) {
            tiles[i].y += gameSpeed;
            
            if(tiles[i].y > HIT_ZONE_Y + 8) {
                tiles[i].active = 0;
                
                if(lives > 0) {
                    lives--;
                }
            }
        } else if(tiles[i].hit) {
            tiles[i].active = 0;
        }
    }
}

// Check collisions
void CheckCollisions(uint8_t key) {
    uint8_t lane;
    int i;
    int16_t distance;
    
    if(key == 0) return;
    
    lane = key - 1;
    if(lane > 2) return;
    
    for(i = 0; i < MAX_TILES; i++) {
        if(tiles[i].active && !tiles[i].hit && tiles[i].lane == lane) {
            distance = abs(tiles[i].y - HIT_ZONE_Y);
            
            if(distance <= PERFECT_ZONE) {
                tiles[i].hit = 1;
                score += 10;
                return;
            } else if(distance <= 8) {
                tiles[i].hit = 1;
                score += 5;
                return;
            }
        }
    }
}

// Draw game over
void DrawGameOver(void) {
    char scoreText[20];
    char* difficultyName;
    uint32_t topScore;
    
    clear_LCD();
    printS_5x7(40, 5, "GAME OVER");
    
    // Show difficulty played
    switch(CURRENT_DIFFICULTY) {
        case 0: difficultyName = " (EASY) "; break;
        case 1: difficultyName = "(MEDIUM)"; break;
        case 2: difficultyName = " (HARD) "; break;
        default: difficultyName = ""; break;
    }
    printS_5x7(45, 18, difficultyName);
    
    sprintf(scoreText, "Score: %u", score);  // No leading zeros for game over score
    printS_5x7(40, 35, scoreText);
    
    if (eeprom_available) {
        topScore = ReadScoreFromEEPROM(1, CURRENT_DIFFICULTY);
        if(score > topScore && score > 0) {
            printS_5x7(20, 42, "NEW HIGH SCORE!");
        }
    }
    
    printS_5x7(30, 54, "Press any key");
}

// Update game
void UpdateGame(void) {
    uint8_t key = ScanKey();
    int i;
    
    switch(gameState) {
        case GAME_MAIN_MENU:
            DrawMainMenu();
            score = 0;
            
            if(key != lastKey && key > 0) {
                // Hidden reset function: Press and hold key 7 in main menu
                if(key == 7 && eeprom_available) {
                    clear_LCD();
                    printS_5x7(20, 15, "RESET ALL SCORES?");
                    printS_5x7(25, 30, "Press 1 = YES");
                    printS_5x7(25, 40, "Press 3 = NO");
                    
                    // Wait for confirmation
                    uint8_t confirmKey = 0;
                    uint8_t lastConfirmKey = 0;
                    int timeout = 0;
                    
                    while(timeout < 500) {  // 5 second timeout
                        confirmKey = ScanKey();
                        if(confirmKey != lastConfirmKey && confirmKey > 0) {
                            if(confirmKey == 1) {
                                // Confirmed reset
                                ResetAllScores();
                                break;
                            }
                            else if(confirmKey == 3) {
                                // Cancelled
                                clear_LCD();
                                printS_5x7(35, 30, "Cancelled");
                                delay_ms(500);
                                break;
                            }
                        }
                        lastConfirmKey = confirmKey;
                        delay_ms(10);
                        timeout++;
                    }
                    
                    // Redraw menu after reset operation
                    clear_LCD();
                }
                else if(key == 1 && currentMenuOption > 1) {
                    currentMenuOption--;
                } else if(key == 3 && currentMenuOption < 3) {
                    currentMenuOption++;
                } else if(key == 2) {
                    DrvGPIO_ClrBit(E_GPC, RIGHT_LED);
                    DrvGPIO_ClrBit(E_GPC, RIGHT_CENTER);
                    DrvGPIO_ClrBit(E_GPC, LEFT_CENTER);
                    DrvGPIO_ClrBit(E_GPC, LEFT_LED);
                    switch(currentMenuOption) {
                        case 1:
                            InitGame();
                            break;
                        case 2:
                            gameState = GAME_DIFFICULTY_MENU;
                            inDifficultyMenu = 1;
                            break;
                        case 3:
                            gameState = GAME_LEADERBOARD;
                            lastLeaderboardPage = 255;  // Force redraw on entry
                            break;
                    }
                }
            }
            break;
            
        case GAME_DIFFICULTY_MENU:
            DrawDifficultyMenu();
            
            if(key != lastKey && key > 0) {
                if(key == 3) {
                    gameState = GAME_MAIN_MENU;
                    inDifficultyMenu = 0;
                    currentMenuOption = 2;
                }
            }
            break;
            
        case GAME_LEADERBOARD:
            DrawLeaderboard();
            
            if(key != lastKey && key > 0) {
                if(key == 3) {
                    // Back to main menu
                    gameState = GAME_MAIN_MENU;
                    currentMenuOption = 3;
                    lastLeaderboardPage = 255;  // Reset so it redraws next time
                }
            }
            break;
            
        case GAME_PLAYING:
            if(lives <= 0) {
                if(score > 0 && eeprom_available) {
                    UpdateLeaderboard(score);
                }
                
                for(i = 0; i < MAX_TILES; i++) {
                    tiles[i].active = 0;
                    tiles[i].hit = 0;
                    tiles[i].y = 0;
                    tiles[i].lane = 0;
                }
                
                gameState = GAME_OVER;
                gameOverCounter = 0;
                break;
            }
            
            if(key != lastKey && key > 0) {
                CheckCollisions(key);
            }
            
            speedCounter++;
            if(speedCounter >= 3) {
                speedCounter = 0;
                MoveTiles();
            }
            
            spawnCounter++;
            if(spawnCounter >= spawnRate) {
                spawnCounter = 0;
                SpawnTile();
                
                if(spawnRate > 15) spawnRate--;
            }
            
            DrawGame();
            break;
            
        case GAME_OVER:
            DrawGameOver();
            gameOverCounter++;
            
            if(gameOverCounter > 20) {
                if(key > 0 && key != lastKey) {
                    ResetGameVariables();
                    gameState = GAME_MAIN_MENU;
                    currentMenuOption = 1;
                    clear_LCD();
                }
            }
            
            if(gameOverCounter > 500) {
                ResetGameVariables();
                gameState = GAME_MAIN_MENU;
                currentMenuOption = 1;
                clear_LCD();
            }
            break;
            
        default:
            gameState = GAME_MAIN_MENU;
            break;
    }
    
    lastKey = key;
}

// Draw game
void DrawGame(void) {
    int i;
    
    clear_LCD();
    DrawLanes();
    DrawHUD();
    
    for(i = 0; i < MAX_TILES; i++) {
        if(tiles[i].active && !tiles[i].hit) {
            DrawTile(tiles[i].lane, tiles[i].y);
        }
    }
}

// Main function
int main(void) {
    int i;
    
    // Turn off all LEDs first
    DrvGPIO_SetBit(E_GPC, LEFT_LED);
    DrvGPIO_SetBit(E_GPC, LEFT_CENTER);
    DrvGPIO_SetBit(E_GPC, RIGHT_CENTER);
    DrvGPIO_SetBit(E_GPC, RIGHT_LED);
    
    UNLOCKREG();
    SYSCLK->PWRCON.XTL12M_EN = 1;
    SYSCLK->CLKSEL0.HCLK_S = 0;
    DrvSYS_Open(48000000);
    LOCKREG();
    
    OpenKeyPad();
    init_LCD();
    clear_LCD();
    InitADC();
    
    for(i = 0; i < 8; i++) {
        DrvGPIO_Open(E_GPE, i, E_IO_OUTPUT);
    }
    for(i = 4; i < 8; i++) {
        DrvGPIO_Open(E_GPC, i, E_IO_OUTPUT);
    }
    
    InitTIMER2();
    srand(12345);
    
    // Set initial difficulty
    CURRENT_DIFFICULTY = 1;  // Default to Medium
    FALL_SPEED = 5;
    lastLeaderboardPage = 255;  // Initialize to force first draw
    
    // Test EEPROM availability (shows message on LCD)
    TestEEPROM();
    
    // Check for reset combination (Keys 1 and 3 pressed together during startup)
    {
        uint8_t key = ScanKey();
        if(key == 1) {
            // Show instruction to press key 3
            clear_LCD();
            printS_5x7(20, 20, "HOLD KEY 1");
            printS_5x7(10, 30, "PRESS KEY 3 TO RESET");
            printS_5x7(15, 40, "or wait to cancel");
            
            int timeout = 0;
            uint8_t resetConfirmed = 0;
            
            // Wait for key 3 or timeout
            while(timeout < 300) {  // About 3 seconds
                key = ScanKey();
                if(key == 3) {
                    resetConfirmed = 1;
                    break;
                }
                else if(key == 0) {
                    // No key pressed, continue waiting
                }
                else {
                    // Different key pressed, cancel
                    break;
                }
                delay_ms(10);
                timeout++;
            }
            
            if(resetConfirmed) {
                ResetAllScores();
            } else {
                clear_LCD();
                printS_5x7(35, 30, "Cancelled");
                delay_ms(500);
            }
        }
    }
    
    ResetGameVariables();
    gameState = GAME_MAIN_MENU;
    
    while(1) {
        UpdateGame();
        for(i = 0; i < 150000; i++);
    }
    
    return 0;
}