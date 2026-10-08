"""Compile upstream's default Pico W path without enabling native Falcon."""
from pathlib import Path
import os,subprocess
r=Path(__file__).resolve().parents[1];env=os.environ.copy()
env.update({'PICO_SDK_PATH':str(r/'pico-sdk'),'PICO_EXTRAS_PATH':str(r/'pico-extras'),'FATFS_SDK_PATH':str(r/'fatfs-sdk'),'BOARD_TYPE':'pico_w','DEBUG_MODE':'0','SIDETNFS_ENABLE_DEBUG':'0','SIDETNFS_ENABLE_DIAG':'0','SIDETNFS_FORCE_CONFIG_RECOVERY':'0','SIDETNFS_ENABLE_SD_SLOT_DUMP':'0','RELEASE_VERSION':'1.1.0-upstream-check','RELEASE_TYPE':'recovery','RELEASE_DATE':'2026-09-23 00:00:00'})
b=r/'build/upstream-check'
subprocess.run(['cmake','-S',str(r/'romemul'),'-B',str(b),'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_POLICY_VERSION_MINIMUM=3.5','-DFETCHCONTENT_FULLY_DISCONNECTED=ON','-DPICO_SDK_FETCH_FROM_GIT=OFF','-DPICO_EXTRAS_FETCH_FROM_GIT=OFF','-DSIDETNFS_NATIVE_FALCON=OFF'],env=env,check=True)
subprocess.run(['cmake','--build',str(b),'--parallel','4'],env=env,check=True)
print('Upstream default Pico W firmware build passed (native Falcon OFF).')
