% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/compute.R
\name{list_compute_functions}
\alias{list_compute_functions}
\title{List available Arrow C++ compute functions}
\usage{
list_compute_functions(pattern = NULL, ...)
}
\arguments{
\item{pattern}{Optional regular expression to filter the function list}

\item{...}{Additional parameters passed to \code{grep()}}
}
\value{
A character vector of available Arrow C++ function names
}
\description{
This function lists the names of all available Arrow C++ library compute functions.
These can be called by passing to \code{\link[=call_function]{call_function()}}, or they can be
called by name with an \code{arrow_} prefix inside a \code{dplyr} verb.
}
\details{
The resulting list describes the capabilities of your \code{arrow} build.
Some functions, such as string and regular expression functions,
require optional build-time C++ dependencies. If your \code{arrow} package
was not compiled with those features enabled, those functions will
not appear in this list.

Some functions take options that need to be passed when calling them
(in a list called \code{options}). These options require custom handling
in C++; many functions already have that handling set up but not all do.
If you encounter one that needs special handling for options, please
report an issue.

Note that this list does \emph{not} enumerate all of the R bindings for these functions.
The package includes Arrow methods for many base R functions that can
be called directly on Arrow objects, as well as some tidyverse-flavored versions
available inside \code{dplyr} verbs.
}
\examples{
available_funcs <- list_compute_functions()
utf8_funcs <- list_compute_functions(pattern = "^UTF8", ignore.case = TRUE)
}
\seealso{
\link{acero} for R bindings for Arrow functions
}
