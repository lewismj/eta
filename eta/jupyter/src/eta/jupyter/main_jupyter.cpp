#include "eta/jupyter/display.h"
#include "eta/jupyter/eta_interpreter.h"

#include <iostream>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    eta::jupyter::EtaInterpreter interpreter;
    const auto display = eta::jupyter::make_plain_display(interpreter.eval("(+ 1 2)"));

    std::cout << "eta_jupyter scaffold ready: " << display.text_repr << '\n';
    return 0;
}
