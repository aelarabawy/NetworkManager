NetworkManager
==============

This is a mirror for the NetworkManager Project. It has the same License as the original code

I first cloned the original project on my machine
  $ git clone git://anongit.freedesktop.org/NetworkManager/NetworkManager.git

Then Created this repo on github

After that I added this repo as a remote for my project
  $ git remote add own https://github.com/aelarabawy/NetworkManager.git

Then I pushed the project to github, this required to pull from it first to be on the tip of the branch, then performed the push 
  $ git pull own master 
  $ git push own master

I will use the original repo to pull any new updates or bug fixes they add $ git pull origin master

While I will use this repo (on Github) to push my own updates $ git push own master

If I find something real useful, I will suggest to add to the original repo

