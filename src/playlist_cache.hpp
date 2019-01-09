/*
 *
 *   Copyright (C) 2018 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#ifndef __playlist_cache_hpp__
#define __playlist_cache_hpp__

#include <map>
#include <string>
#include <memory>

namespace Buffers {
    class Playlist;

    class Segment
    {
    public:
        //            const uint8_t* Pop(size_t requesred, size_t*  actual);
        size_t Read(uint8_t* buffer, size_t size);
        size_t Seek(size_t position);
        size_t Position() const  {return _begin - &_data[0];}
        size_t BytesReady() const {return  _size - Position();}
        float Bitrate() const { return  _duration == 0.0 ? 0.0 : _size/_duration;}
        float Duration() const {return _duration;}
        size_t Length() const {return _size;}
    protected:
        Segment(float duration);
        Segment(const uint8_t* buffer, size_t size, float duration);
        ~Segment();
        uint8_t* _data;
        size_t _size;
        const uint8_t* _begin;
        const float _duration;
    };
    
    class MutableSegment : public Segment {
        MutableSegment(const std::string& u, float duration)
        : Segment(duration)
        , url(u)
        {}
    public:
        const std::string url;
        void Push(const uint8_t* buffer, size_t size);
    private: // ???
        ~MutableSegment(){};
    };


    class PlaylistCache {
        
    public:
        PlaylistCache(const std::string &streamUrl);
        ~PlaylistCache();
        MutableSegment* SegmentToFillAfter(size_t position);
        void SegmentReady(MutableSegment* segment);
        Segment* SegmentAt(size_t position);
        bool HasSegmentsToFill() const;
        bool IsEof(size_t position) const;
        float Bitrate() const { return m_totalDuration == 0 ? 0.0 : m_totalLength / m_totalDuration;}
    private:
       
        // key is time offset from the beginning (durations sum)
        typedef std::map<float, Segment*>  TSegments;

        Playlist* m_playlist;
        std::list<std::unique_ptr<MutableSegment>> m_segmentsSwamp;
        TSegments m_segments;
        int64_t m_totalLength;
        float m_totalDuration;
        
    };
    
}
#endif /* __playlist_cache_hpp__ */
