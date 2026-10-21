<img width="128" height="128" alt="PS5Save" src="https://github.com/user-attachments/assets/dbdbdd48-14d2-46fa-8a6f-189ede65da7b" /> 

# PS5Save - Automatic game save and file backups!
Easy to use USB save backup utility for jailbroken PS5s that helps you prevent data loss!  
- Fully automatic with daily snapshots, supports PS5 saves, PS4 saves, emulator saves and other files
<img width="1280" height="720" alt="PS5Save notification screenshot" src="https://github.com/user-attachments/assets/cbef0c27-b218-4da1-943c-f6c0da6e0ef4" />


# Guide
- Get a fast USB storage device that is big enough to fit all of your saves (formatted as exFAT)
- Create a `backup.conf` file on the root of your USB
- Fill `backup.conf` with your personalized values (look at Backup Configuration section)
- Plug in into your PS5
- Push the backup.elf payload to your PS5 using a network loader or an autoloader of your choice

PS4/PS5 saves will backed up while in-game except for the game you're currently playing it will be backed up after you exit the title *(since version v2)*

# Backup Configuration
Example: 
```
# Delay between backup passes in seconds (int, min 30 max 3600)
backup_interval_seconds=30

# Enable decrypted PS4 save backups (false, true)
backup_ps4_saves=false

# Enable decrypted PS5 save backups (false, true)
backup_ps5_saves=false

# Maximum combined save-image size per title in MB (int)
max_title_size_mb=4096

# Number of dated snapshots retained, values 0 thru 10 are valid; 0 means disabled (int)
daily_snapshots_to_keep=3

# File backup section with most popular emulators listed

# PS5SX2
/data/PCSX2/Mcd001.ps2
/data/PCSX2/Mcd002.ps2

# PS5SX3
/data/homebrew/PPSA99303/rpcs3/dev_hdd0/home/

# Porpoise
/data/porpoise/saves/

# ProsperoEden
/data/prosperoeden/user/nand
```


## Credits
- n0llptr — [Playstation 5 Save Mounter](https://github.com/n0llptr/Playstation-5-Save-Mounter)
- earthonion — loading game titles code and save creation: [Garlic SaveMgr](https://git.etawen.dev/earthonion/garlic-savemgr)
- cow — save mounting research using internal PS5 `sceFs*` functions

## Building

Requires Linux (for example Ubuntu 24.04) and the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

Install the build tools

```sh
sudo apt update
sudo apt install make clang-18 lld-18 llvm-18 wget unzip
```

Download and extract the SDK

```sh
wget https://github.com/ps5-payload-dev/sdk/releases/download/v0.44/ps5-payload-sdk.zip
unzip ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK="$(pwd)/ps5-payload-sdk"
export LLVM_CONFIG=llvm-config-18
```

Open the PS5Save source directory, then run:

```sh
make
```

The compiled payload is `backup.elf`.

To rebuild after changing the source:

```sh
make clean
make
```
