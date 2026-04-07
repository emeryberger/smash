.PHONY: all macros figures paper clean

all: macros figures paper

# Generate LaTeX macros from benchmark results JSON
macros: results_macros.tex

results_macros.tex: gen_macros.py $(wildcard ../paper_results/*.json)
	python3 gen_macros.py

# Regenerate all figures from benchmark results
figures:
	cd figures && python3 plot_all.py
	cd figures && python3 plot_rss_timeline.py
	cd figures && python3 plot_cdf.py

# Build the paper PDF via latexrun
paper: results_macros.tex
	./latexrun -o paper.pdf --latex-args="-shell-escape" -Wall paper.tex

clean:
	./latexrun --clean-all
	rm -f results_macros.tex
