; Eta prelude

; negation.
(defun (not x)
    (if (== #t x)
        (#f)
        (#t)
    ))

; list functions.
(define (nil) '())

(defun (empty xs) (if (== xs nil) (#t) (#f)))

; length of list.
(defun (len xs)
  (if (== xs nil) 
    (0) 
    (+ 1 (len (tail xs)))
  )) 

; first element of a list.
(defun (fst xs) ( eval (head xs) ))

; drop n-elements from a list.
(defun (drop n xs)
    (if (== n 0)
        (xs)
        (drop (- n 1) (tail xs))
     ))

; fold a list.
(defun (foldl f z xs)
    (if (== xs nil)
        (z)
        (foldl f (f z (fst xs)) (tail xs))
    ))

; map over the elements of a list.
(defun (map f xs)
    ((if (== xs nil)
        (nil)
        (join (list (f (fst xs))) (map f (tail xs))))
    ))

; filter elements of list.
(defun (filter f xs)
        (if (== xs nil)
            (nil)
            (join (if (f (fst xs)) (head xs) (nil)) (filter f (tail xs)))
        ))

; quicksort
(defun (sort xs)
    (if (<= (len xs) 1)
        (xs)
        (
            ( let (pivot (fst xs)) )
            (join
                (sort (filter (lambda (n) (> pivot n)) xs))
                (pivot)
                (sort (tail (filter (lambda (n) (<= pivot n)) xs)))
               )
          )
      ))

; math constants
(define (_pi) (3.141592653589793))
(define (_tau) (6.283185307179586))
(define (_e) (2.718281828459045))

(defun (odd n) (if (== 0 (% n 2)) (#f) (#t)))
(defun (even n) (not (odd n)))


