# Peer to Perr File Transfer (CUHK CSCI4430)

# Instruction
Compile:
```
make
```
Before transfer, generate a torrent:
```
./tgen <ip> <port> <filePath> <filename>
```
* ip: tracker ip
* port: tracker port
* filePath: the file to be transferred
* filename: torrent file name

And then run tracker:
```
./tracker <portno>
```
* portno: tracker port

Run peer:
```
./peer <add/vampire/seed/subseed> <torrent> <ip> <filePath>
```
* add/vampire/seed/subseed: different mode
	- add: download and upload
	- vampire: download only
	- seed: upload only
	- subseed: upload partial only
* torrent: torrent file
* ip: peer ip
* filePath: the downloaded file to be stored

# Screenshot
![ss1](/screenshots/ss1.png)
![ss2](/screenshots/ss2.png)
![ss3](/screenshots/ss3.png)
![ss4](/screenshots/ss4.png)
