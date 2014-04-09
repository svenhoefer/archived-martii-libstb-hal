/*
 * linuxdvb output/writer handling
 *
 * Copyright (C) 2010  duckbox
 * Copyright (C) 2014  martii   (based on code from libeplayer3)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <sys/prctl.h>

#include "player.h"
#include "misc.h"

#define cMaxSpeed_ff   128	/* fixme: revise */
#define cMaxSpeed_fr   -320	/* fixme: revise */

Player::Player()
{
	input.player = this;
	output.player = this;
	manager.player = this;
	hasThreadStarted = false;

	isPaused = false;
	isPlaying = false;
	isForwarding = false;
	isBackWard = false;
	isSlowMotion = false;
	Speed = 0;
}

void *Player::playthread(void *arg)
{
	char threadname[17];
	strncpy(threadname, __func__, sizeof(threadname));
	threadname[16] = 0;
	prctl(PR_SET_NAME, (unsigned long) &threadname);

	Player *player = (Player *) arg;
	player->hasThreadStarted = true;
	player->input.Play();
	player->hasThreadStarted = false;
	player->Stop();
	pthread_exit(NULL);
}

bool Player::Open(const char *Url, bool _noprobe)
{
	fprintf(stderr, "URL=%s\n", Url);

	isHttp = false;
	noprobe = _noprobe;
	abortRequested = false;

	manager.clearTracks();

	if (*Url == '/') {
		url = "file://";
		url += Url;
	} else if (!strncmp("mms://", Url, 6)) {
		url = "mmst";
		url += Url + 3;
		isHttp = true;
	} else if (strstr(Url, "://")) {
		url = Url;
	} else {
		fprintf(stderr, "%s %s %d: Unknown stream (%s)\n", __FILE__, __func__, __LINE__, Url);
		return false;
	}

	if (!input.Init(url.c_str()))
		return false;

	return true;
}

bool Player::Close()
{
	bool ret = true;

	isPaused = false;
	isPlaying = false;
	isForwarding = false;
	isBackWard = false;
	isSlowMotion = false;
	Speed = 0;
	url.clear();

	return ret;
}

bool Player::Play()
{
	bool ret = true;

	if (!isPlaying) {
		output.AVSync(true);

		ret = output.Play();

		if (ret) {
			isPlaying = true;
			isPaused = false;
			isForwarding = false;
			if (isBackWard) {
				isBackWard = false;
				output.Mute(false);
			}
			isSlowMotion = false;
			Speed = 1;

			if (!hasThreadStarted) {
				int err = pthread_create(&playThread, NULL, playthread, this);

				if (err) {
					fprintf(stderr, "%s %s %d: pthread_create: %d (%s)\n", __FILE__, __func__, __LINE__, err, strerror(err));
					ret = false;
					isPlaying = false;
				} else {
					pthread_detach(playThread);
				}
			}
		}

	} else {
		fprintf(stderr,"playback already running\n");
		ret = false;
	}
	return ret;
}

bool Player::Pause()
{
	bool ret = true;

	if (isPlaying && !isPaused) {

		if (isSlowMotion)
			output.Clear();

		output.Pause();

		isPaused = true;
		//isPlaying  = 1;
		isForwarding = false;
		if (isBackWard) {
			isBackWard = false;
			output.Mute(false);
		}
		isSlowMotion = false;
		Speed = 1;
	} else {
		fprintf(stderr,"playback not playing or already in pause mode\n");
		ret = false;
	}
	return ret;
}

bool Player::Continue()
{
	int ret = true;

	if (isPlaying && (isPaused || isForwarding || isBackWard || isSlowMotion)) {

		if (isSlowMotion)
			output.Clear();

		output.Continue();

		isPaused = false;
		//isPlaying  = 1;
		isForwarding = false;
		if (isBackWard) {
			isBackWard = false;
			output.Mute(false);
		}
		isSlowMotion = false;
		Speed = 1;
	} else {
		fprintf(stderr,"continue not possible\n");
		ret = false;
	}

	return ret;
}

bool Player::Stop()
{
	bool ret = true;

	if (isPlaying) {

		isPaused = false;
		isPlaying = false;
		isForwarding = false;
		if (isBackWard) {
			isBackWard = false;
			output.Mute(false);
		}
		isSlowMotion = false;
		Speed = 0;

		output.Stop();
		input.Stop();

	} else {
		fprintf(stderr,"stop not possible\n");
		ret = false;
	}

	while (hasThreadStarted)
		usleep(100000);

	return ret;
}

bool Player::FastForward(int speed)
{
	int ret = true;

	/* Audio only forwarding not supported */
	if (input.videoTrack && !isHttp && !isBackWard && (!isPaused ||  isPlaying)) {

		if ((speed <= 0) || (speed > cMaxSpeed_ff)) {
			fprintf(stderr, "speed %d out of range (1 - %d) \n", speed, cMaxSpeed_ff);
			return false;
		}

		isForwarding = 1;
		Speed = speed;
		output.FastForward(speed);
		} else {
		fprintf(stderr,"fast forward not possible\n");
		ret = false;
	}

	return ret;
}

bool Player::FastBackward(int speed)
{
	bool ret = true;

	/* Audio only reverse play not supported */
	if (input.videoTrack && !isForwarding && (!isPaused || isPlaying)) {

	if ((speed > 0) || (speed < cMaxSpeed_fr)) {
		fprintf(stderr, "speed %d out of range (0 - %d) \n", speed, cMaxSpeed_fr);
		return false;
	}

	if (speed == 0) {
		isBackWard = false;
		Speed = 0;	/* reverse end */
	} else {
		Speed = speed;
		isBackWard = true;
	}

	output.Clear();
#if 0
	if (output->Command(player, OUTPUT_REVERSE, NULL) < 0) {
		fprintf(stderr,"OUTPUT_REVERSE failed\n");
		isBackWard = false;
		Speed = 1;
		ret = false;
	}
#endif
	} else {
		fprintf(stderr,"fast backward not possible\n");
		ret = false;
	}

	if (isBackWard)
		output.Mute(true);

	return ret;
}

bool Player::SlowMotion(int repeats)
{
	if (input.videoTrack && !isHttp && isPlaying) {
		if (isPaused)
			Continue();

		switch (repeats) {
			case 2:
			case 4:
			case 8:
				isSlowMotion = true;
				break;
			default:
				repeats = 0;
		}

		output.SlowMotion(repeats);
		return true;
	}
	fprintf(stderr, "slowmotion not possible\n");
	return false;
}

bool Player::Seek(float pos, bool absolute)
{
	output.Clear();
	return input.Seek(pos, absolute);
}

bool Player::GetPts(int64_t &pts)
{
	pts = INVALID_PTS_VALUE;
	return isPlaying && output.GetPts(pts);
}

bool Player::GetFrameCount(int64_t &frameCount)
{
	return isPlaying && output.GetFrameCount(frameCount);
}

bool Player::GetDuration(double &duration)
{
	duration = -1;
	return isPlaying && input.GetDuration(duration);
}

bool Player::SwitchVideo(int pid)
{
	Track *track = manager.getVideoTrack(pid);
	return input.SwitchVideo(track);
}

bool Player::SwitchAudio(int pid)
{
	Track *track = manager.getAudioTrack(pid);
	return input.SwitchAudio(track);
}

bool Player::SwitchSubtitle(int pid)
{
	Track *track = manager.getSubtitleTrack(pid);
	return input.SwitchSubtitle(track);
}

bool Player::SwitchTeletext(int pid)
{
	Track *track = manager.getTeletextTrack(pid);
	return input.SwitchTeletext(track);
}

bool Player::GetMetadata(std::vector<std::string> &keys, std::vector<std::string> &values)
{
	return input.GetMetadata(keys, values);
}

bool Player::GetChapters(std::vector<int> &positions, std::vector<std::string> &titles)
{
	positions.clear();
	titles.clear();
	input.UpdateTracks();
	OpenThreads::ScopedLock<OpenThreads::Mutex> m_lock(chapterMutex);
	for (std::vector<Chapter>::iterator it = chapters.begin(); it != chapters.end(); ++it) {
		positions.push_back(1000 * it->start);
		titles.push_back(it->title);
	}
	return true;
}

void Player::SetChapters(std::vector<Chapter> &Chapters)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> m_lock(chapterMutex);
	chapters = Chapters;
}

void Player::RequestAbort()
{
	abortRequested = true;
}

int Player::GetVideoPid()
{
	Track *track = input.videoTrack;
	if (track)
		return track->pid;
	return -1;
}

int Player::GetAudioPid()
{
	Track *track = input.audioTrack;
	if (track)
		return track->pid;
	return -1;
}

int Player::GetSubtitlePid()
{
	Track *track = input.subtitleTrack;
	if (track)
		return track->pid;
	return -1;
}

int Player::GetTeletextPid()
{
	Track *track = input.teletextTrack;
	if (track)
		return track->pid;
	return -1;
}