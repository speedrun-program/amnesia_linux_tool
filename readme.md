
# STARTING AMNESIA WITH THE TOOL

- write something in shared_memory_name.txt

- use LD_PRELOAD to start Amnesia with the tool.
  Use the 32-bit tool with 32-bit Amnesia and the 64-bit tool with 64-bit Amnesia.

    LD_PRELOAD='tool file path' 'Amnesia file path'

- make sure you're running Amnesia from the same directory as shared_memory_name.txt, files_and_delays.txt, and flashback_names.txt.

# INSTRUCTIONS

how to make the game skip flashbacks in load screens:
- **CHECK IF THE MODERATORS ALLOW THIS.**
  when this was written, the mods said they wouldn't let people use this feature.
  check if they allow this. If they don't, but you want to use it, tell them you think they should allow it.
- in settings.txt, set "skip flashbacks" to "y".

how to make the game wait through flashbacks in load screens:
- in settings.txt, set "delay flashbacks" to "y".
- if "skip flashbacks" is set to "y", this setting is ignored.

how to have load delays for maps in quitouts which you quitout in:
- in settings.txt, set "delay files" to "y".

how to turn off the load delays for maps in quitouts which you quitout in:
- in settings.txt, set "delay files" to "n".

how to adjust delays or add maps in files_and_delays.txt:
- between the slashes, put the map name at the start, the delay in milliseconds in the middle, and a dash at the end.
  e.g.: 12_storage.hps / 1575 / -
  this will give the loads for the Storage map a delay of 1575 milliseconds.
- remember to delay the .hps files instead of .map files or .map_cache files, except for menu_bg.map.
  The reason to do this is because .hps files don't trigger delays during quickloads or when opening
  a map folder from the debug menu.

how to add more flashbacks to wait through or skip:
- add the sound files to flashback_names.txt.
- the sound files used by each flashback in are listed in the .flash files in \Amnesia The Dark Descent\flashbacks.
- in English, the sound files are in \Amnesia The Dark Descent\lang\eng\voices\flashbacks.
  in Russian, the sound files are in \Amnesia The Dark Descent\lang\rus\voices\flashbacks.
  you can listen to them to check if they're the ones you want to skip.


**You most likely don't need to read the next part.**

files_and_delays.txt syntax:

On each line in files_and_delays.txt, write file name and delay sequence separated by a forward slash.
Delay time is in milliseconds.
Write a dash character (this character: -) as the last part of the delay sequence if you want it to restart.
If a dash character is written as the first part of the sequence, the file will restart every delay sequence
when it's loaded.

EXAMPLE:

    game_map1.hps / 1000 / 2500
    game_map2.hps / 2000 / 3000 / 4000 / -
    game_map3.hps / -

game_map1.hps will be delayed 1 second on its 1st load and 2.5 seconds on its 2nd load.
game_map2.hps will be delay 2 seconds, 3 seconds, and 4 seconds in a cycle.
game_map3.hps will restart the other maps' sequences whenever it's loaded.
