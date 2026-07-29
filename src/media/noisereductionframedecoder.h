#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>

namespace MicArray {

struct NoiseReductionInputFrame {
    bool valid = false;
    QString error;
    std::array<QVector<float>, 4> channels;
};

class NoiseReductionFrameDecoder
{
public:
    NoiseReductionInputFrame decode(const QByteArray &payload) const;
};

} // namespace MicArray
