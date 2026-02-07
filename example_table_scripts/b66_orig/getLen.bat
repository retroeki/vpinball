@echo Off
@for %%A IN (*.mp4) DO ffprobe -i "%%A" -show_entries format=duration,filename -v quiet -of csv=p=0