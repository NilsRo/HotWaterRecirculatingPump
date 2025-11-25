#ifndef SIMPLE_AVERAGE_H
#define SIMPLE_AVERAGE_H

#include <Arduino.h>

class SimpleAverage
{
private:
    float *buffer; // Ringpuffer
    int size;      // Fenstergröße
    int index;     // Aktueller Index
    int count;     // Anzahl Werte
    float sum;     // Laufende Summe

public:
    // Konstruktor
    SimpleAverage(int windowSize)
    {
        size = windowSize;
        buffer = new float[size];
        index = 0;
        count = 0;
        sum = 0.0f;
        for (int i = 0; i < size; i++)
            buffer[i] = 0.0f;
    }

    // Destruktor
    ~SimpleAverage()
    {
        delete[] buffer;
    }

    // Wert hinzufügen
    void addValue(float value)
    {
        sum -= buffer[index];
        buffer[index] = value;
        sum += value;

        index = (index + 1) % size;
        if (count < size)
            count++;
    }

    // Durchschnitt berechnen
    float getAverage() const
    {
        if (count == 0)
            return 0.0f;
        return sum / count;
    }

    // Reset
    void reset()
    {
        for (int i = 0; i < size; i++)
            buffer[i] = 0.0f;
        sum = 0.0f;
        index = 0;
        count = 0;
    }

    // --- Operator Overloads ---
    // Cast zu float → erlaubt direkte Nutzung in Rechnungen
    operator float() const
    {
        return getAverage();
    }

    // Vergleichsoperatoren
    bool operator<(float rhs) const { return getAverage() < rhs; }
    bool operator>(float rhs) const { return getAverage() > rhs; }
    bool operator<=(float rhs) const { return getAverage() <= rhs; }
    bool operator>=(float rhs) const { return getAverage() >= rhs; }
    bool operator==(float rhs) const { return getAverage() == rhs; }
    bool operator!=(float rhs) const { return getAverage() != rhs; }
};

#endif

// #include "SimpleAverage.h"

// SimpleAverage avg(10);

// void setup()
// {
//     Serial.begin(115200);
// }

// void loop()
// {
//     float sensorValue = analogRead(34);
//     avg.addValue(sensorValue);

//     // Direkt rechnen
//     float doubled = avg * 2; // Cast zu float → funktioniert
//     float offset = avg + 5;

//     // Direkt vergleichen
//     if (avg > 2000)
//     {
//         Serial.println("Sensorwert (geglättet) ist hoch!");
//     }

//     Serial.print("Smoothed: ");
//     Serial.println((float)avg); // expliziter Cast oder implizit
//     delay(100);
// }