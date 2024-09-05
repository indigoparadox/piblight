
# vim: ft=make noexpandtab

piblight: piblight.c
	$(CC) -o $@ $< -lmosquitto -DDEBUG

clean:
	rm -f piblight

