/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2019 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QtTest/QtTest>
#include <QObject>
#include "errorlogger.h"

class BenchmarkSimple : public QObject, public ErrorLogger {
    Q_OBJECT

private slots:
    void tokenize();
    void simplify();
    void tokenizeAndSimplify();

private:
    // Empty implementations of ErrorLogger methods.
    // We don't care about the output in the benchmark tests.
    void reportOut(const std::string & /*outmsg*/, Color /*c*/ = Color::Reset) override {}
    void reportErr(const ErrorMessage & /*msg*/) override {}
    void bughuntingReport(const std::string & /*str*/) override {}
};
